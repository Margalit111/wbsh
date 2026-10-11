/**
 * @file huffman.cpp
 * @brief Package-merge: the boundary-free textbook form, which is small
 *        and exact for alphabets of a few hundred symbols.
 */

#include "huffman.h"

#include <algorithm>

namespace wbsh {

	namespace huffman_detail {

		struct MergeNode {
			std::uint64_t weight = 0;
			int symbol = -1;
			int left = -1;
			int right = -1;
		};

		struct WeightOrder {
			const std::vector<MergeNode>* nodes;

			bool operator()(int a, int b) const {
				return (*nodes)[static_cast<std::size_t>(a)].weight
					< (*nodes)[static_cast<std::size_t>(b)].weight;
			}
		};

	}  // namespace huffman_detail

	using huffman_detail::MergeNode;
	using huffman_detail::WeightOrder;

	static std::vector<int> leafList(const std::vector<std::uint32_t>& frequencies,
	                                 std::vector<MergeNode>& nodes) {
		std::vector<int> leaves;
		for (std::size_t symbol = 0; symbol < frequencies.size(); ++symbol) {
			if (frequencies[symbol] == 0) continue;

			MergeNode leaf;
			leaf.weight = frequencies[symbol];
			leaf.symbol = static_cast<int>(symbol);
			leaves.push_back(static_cast<int>(nodes.size()));
			nodes.push_back(leaf);
		}

		std::stable_sort(leaves.begin(), leaves.end(), WeightOrder{ &nodes });
		return leaves;
	}

	static std::vector<int> packagePairs(const std::vector<int>& list,
	                                     std::vector<MergeNode>& nodes) {
		std::vector<int> packages;
		for (std::size_t k = 0; k + 1 < list.size(); k += 2) {
			MergeNode package;
			package.weight = nodes[static_cast<std::size_t>(list[k])].weight
				+ nodes[static_cast<std::size_t>(list[k + 1])].weight;
			package.left = list[k];
			package.right = list[k + 1];
			packages.push_back(static_cast<int>(nodes.size()));
			nodes.push_back(package);
		}

		return packages;
	}

	static void countLeaves(const std::vector<MergeNode>& nodes, int index,
	                        std::vector<std::uint8_t>& lengths) {
		std::vector<int> pending = { index };
		while (!pending.empty()) {
			const MergeNode& node = nodes[static_cast<std::size_t>(pending.back())];
			pending.pop_back();
			if (node.symbol >= 0) {
				++lengths[static_cast<std::size_t>(node.symbol)];
				continue;
			}

			pending.push_back(node.left);
			pending.push_back(node.right);
		}
	}

	std::vector<std::uint8_t> limitedCodeLengths(const std::vector<std::uint32_t>& frequencies,
	                                             int max_length) {
		std::vector<std::uint8_t> lengths(frequencies.size(), 0);
		std::vector<MergeNode> nodes;
		const std::vector<int> leaves = leafList(frequencies, nodes);
		if (leaves.empty()) return lengths;
		if (leaves.size() == 1) {
			const MergeNode& only = nodes[static_cast<std::size_t>(leaves[0])];
			lengths[static_cast<std::size_t>(only.symbol)] = 1;
			return lengths;
		}

		std::vector<int> list = leaves;
		for (int level = 1; level < max_length; ++level) {
			std::vector<int> merged = packagePairs(list, nodes);
			merged.insert(merged.end(), leaves.begin(), leaves.end());
			std::stable_sort(merged.begin(), merged.end(), WeightOrder{ &nodes });
			list = merged;
		}

		const std::size_t selected = 2 * leaves.size() - 2;
		for (std::size_t k = 0; k < selected && k < list.size(); ++k) {
			countLeaves(nodes, list[k], lengths);
		}

		return lengths;
	}

	std::vector<std::uint32_t> canonicalCodes(const std::vector<std::uint8_t>& lengths) {
		std::uint8_t longest = 0;
		for (const std::uint8_t length : lengths) longest = (std::max)(longest, length);

		std::vector<std::uint32_t> count(static_cast<std::size_t>(longest) + 1, 0);
		for (const std::uint8_t length : lengths) {
			if (length != 0) ++count[length];
		}

		std::vector<std::uint32_t> next(count.size() + 1, 0);
		std::uint32_t code = 0;
		for (std::size_t bits = 1; bits < count.size(); ++bits) {
			code = (code + count[bits - 1]) << 1;
			next[bits] = code;
		}

		std::vector<std::uint32_t> codes(lengths.size(), 0);
		for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
			if (lengths[symbol] != 0) codes[symbol] = next[lengths[symbol]]++;
		}

		return codes;
	}

}  // namespace wbsh
