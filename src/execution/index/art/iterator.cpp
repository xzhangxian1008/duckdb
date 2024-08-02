#include "duckdb/execution/index/art/iterator.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/execution/index/art/prefix.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// IteratorKey
//===--------------------------------------------------------------------===//

bool IteratorKey::Contains(const ARTKey &key) const {
	if (Size() < key.len) {
		return false;
	}
	for (idx_t i = 0; i < key.len; i++) {
		if (key_bytes[i] != key.data[i]) {
			return false;
		}
	}
	return true;
}

bool IteratorKey::GreaterThan(const ARTKey &key, const bool equal) const {
	for (idx_t i = 0; i < MinValue<idx_t>(Size(), key.len); i++) {
		if (key_bytes[i] > key.data[i]) {
			return true;
		} else if (key_bytes[i] < key.data[i]) {
			return false;
		}
	}
	if (equal) {
		// Returns true, if current_key is greater than key.
		return Size() > key.len;
	}
	// Returns true, if current_key and key match or current_key is greater than key.
	return Size() >= key.len;
}

//===--------------------------------------------------------------------===//
// Iterator
//===--------------------------------------------------------------------===//

bool Iterator::Scan(const ARTKey &upper_bound, const idx_t max_count, unsafe_vector<row_t> &row_ids, const bool equal) {
	bool has_next;
	do {
		// An empty upper bound indicates that no upper bound exists.
		if (!upper_bound.Empty() && !inside_gate) {
			if (current_key.GreaterThan(upper_bound, equal)) {
				return true;
			}
		}

		switch (last_leaf.GetType()) {
		case NType::LEAF_INLINED:
			if (row_ids.size() + 1 > max_count) {
				return false;
			}
			row_ids.push_back(last_leaf.GetRowId());
			break;
		case NType::LEAF:
			if (!Leaf::DeprecatedGetRowIds(*art, last_leaf, row_ids, max_count)) {
				return false;
			}
			break;
		case NType::NODE_7_LEAF:
		case NType::NODE_15_LEAF:
		case NType::NODE_256_LEAF: {
			uint8_t byte = 0;
			while (last_leaf.GetNextByte(*art, byte)) {
				if (row_ids.size() + 1 > max_count) {
					return false;
				}
				row_id[sizeof(row_t) - 1] = byte;
				ARTKey key(&row_id[0], sizeof(row_t));
				row_ids.push_back(key.GetRowID());
				if (byte == NumericLimits<uint8_t>::Maximum()) {
					break;
				}
				byte++;
			}
			break;
		}
		case NType::PREFIX_INLINED: {
			Prefix prefix(*art, last_leaf);
			for (idx_t i = 0; i < prefix.data[Prefix::Count(*art)]; i++) {
				row_id[i + nested_depth] = prefix.data[i];
			}
			ARTKey key(&row_id[0], sizeof(row_t));
			row_ids.push_back(key.GetRowID());
			break;
		}
		default:
			throw InternalException("Invalid leaf type for index scan.");
		}

		has_next = Next();
	} while (has_next);
	return true;
}

void Iterator::FindMinimum(const Node &node) {
	D_ASSERT(node.HasMetadata());

	// Found the minimum.
	if (node.IsAnyLeaf()) {
		last_leaf = node;
		return;
	}

	// We are passing a gate node.
	if (node.IsGate()) {
		D_ASSERT(!inside_gate);
		inside_gate = true;
		nested_depth = 0;
	}

	// Traverse the prefix.
	if (node.GetType() == NType::PREFIX) {
		auto &prefix = Node::Ref<const Prefix>(*art, node, NType::PREFIX);
		for (idx_t i = 0; i < prefix.data[Prefix::Count(*art)]; i++) {
			current_key.Push(prefix.data[i]);
			if (inside_gate) {
				row_id[nested_depth] = prefix.data[i];
				nested_depth++;
			}
		}
		nodes.emplace(node, 0);
		return FindMinimum(*prefix.ptr);
	}

	// Go to the leftmost entry in the current node.
	uint8_t byte = 0;
	auto next = node.GetNextChild(*art, byte);
	D_ASSERT(next);

	// Recurse on the leftmost node.
	current_key.Push(byte);
	if (inside_gate) {
		row_id[nested_depth] = byte;
		nested_depth++;
	}
	nodes.emplace(node, byte);
	FindMinimum(*next);
}

bool Iterator::LowerBound(const Node &node, const ARTKey &key, const bool equal, idx_t depth) {
	if (!node.HasMetadata()) {
		return false;
	}

	// We found any leaf node, or a gate.
	if (node.IsAnyLeaf() || node.IsGate()) {
		D_ASSERT(!inside_gate);
		D_ASSERT(current_key.Size() == key.len);
		if (!equal && current_key.Contains(key)) {
			return Next();
		}

		if (node.IsGate()) {
			FindMinimum(node);
		} else {
			last_leaf = node;
		}
		return true;
	}

	D_ASSERT(!node.IsGate());
	if (node.GetType() != NType::PREFIX) {
		auto next_byte = key[depth];
		auto child = node.GetNextChild(*art, next_byte);

		// The key is greater than any key in this subtree.
		if (!child) {
			return Next();
		}

		current_key.Push(next_byte);
		nodes.emplace(node, next_byte);

		// We return the minimum because all keys are greater than the lower bound.
		if (next_byte > key[depth]) {
			FindMinimum(*child);
			return true;
		}

		// We recurse into the child.
		return LowerBound(*child, key, equal, depth + 1);
	}

	// Push back all prefix bytes.
	auto &prefix = Node::Ref<const Prefix>(*art, node, NType::PREFIX);
	for (idx_t i = 0; i < prefix.data[Prefix::Count(*art)]; i++) {
		current_key.Push(prefix.data[i]);
	}
	nodes.emplace(node, 0);

	// We compare the prefix bytes with the key bytes.
	for (idx_t i = 0; i < prefix.data[Prefix::Count(*art)]; i++) {
		// We found a prefix byte that is less than its corresponding key byte.
		// I.e., the subsequent node is lesser than the key. Thus, the next node
		// is the lower bound.
		if (prefix.data[i] < key[depth + i]) {
			return Next();
		}

		// We found a prefix byte that is greater than its corresponding key byte.
		// I.e., the subsequent node is greater than the key. Thus, the minimum is
		// the lower bound.
		if (prefix.data[i] > key[depth + i]) {
			FindMinimum(*prefix.ptr);
			return true;
		}
	}

	// The prefix matches the key. We recurse into the child.
	depth += prefix.data[Prefix::Count(*art)];
	return LowerBound(*prefix.ptr, key, equal, depth);
}

bool Iterator::Next() {
	while (!nodes.empty()) {
		auto &top = nodes.top();
		D_ASSERT(!top.node.IsAnyLeaf());

		if (top.node.GetType() == NType::PREFIX) {
			PopNode();
			continue;
		}

		if (top.byte == NumericLimits<uint8_t>::Maximum()) {
			// No more children of this node.
			// Move up the tree by popping the key byte of the current node.
			PopNode();
			continue;
		}

		top.byte++;
		auto next_node = top.node.GetNextChild(*art, top.byte);
		if (!next_node) {
			// No more children of this node.
			// Move up the tree by popping the key byte of the current node.
			PopNode();
			continue;
		}

		current_key.Pop(1);
		current_key.Push(top.byte);
		if (inside_gate) {
			row_id[nested_depth - 1] = top.byte;
		}

		FindMinimum(*next_node);
		return true;
	}
	return false;
}

void Iterator::PopNode() {
	// We are popping a gate node.
	if (nodes.top().node.IsGate()) {
		D_ASSERT(inside_gate);
		inside_gate = false;
	}

	// Pop the byte and the node.
	if (nodes.top().node.GetType() != NType::PREFIX) {
		current_key.Pop(1);
		if (inside_gate) {
			nested_depth--;
		}
		nodes.pop();
		return;
	}

	// Pop all prefix bytes and the node.
	auto &prefix = Node::Ref<const Prefix>(*art, nodes.top().node, NType::PREFIX);
	auto prefix_byte_count = prefix.data[Prefix::Count(*art)];
	current_key.Pop(prefix_byte_count);
	if (inside_gate) {
		nested_depth -= prefix_byte_count;
	}
	nodes.pop();
}

} // namespace duckdb
