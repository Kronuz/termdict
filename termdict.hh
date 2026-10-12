/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// A block-tree sorted string table: sorted byte-string keys mapped to opaque value
// bytes, stored as prefix-compressed blocks plus a small FST over each block's first
// key. A lookup is one FST floor, a binary search over the block's restart points,
// and at most `restart_interval` entry decodes.
//
// Block layout (all integers LEB128):
//   [block length][entry count][restart count][restart offsets...][entries...]
// Entry: [shared prefix length][suffix length][suffix][value length][value]
// Restart points have a shared length of 0; offsets are relative to the first entry.

#pragma once

#include "fst.hh"
#include "varint.hh"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace termdict {

inline constexpr std::size_t restart_interval = 16;
inline constexpr std::size_t min_block_entries = 25;
inline constexpr std::size_t max_block_entries = 48;
inline constexpr std::size_t target_block_bytes = 1024;

// Thrown when the bytes are not a well-formed dictionary; callers fence the data.
struct corrupt : std::runtime_error {
	corrupt() : std::runtime_error("termdict: corrupt block") {}
};

// The builder's output: block bytes and the FST index over them.
struct tables {
	std::string blocks;
	std::string index;
};

// Builds from keys in strictly increasing byte order.
class builder {
public:
	void add(std::string_view key, std::string_view value) {
		if (count_ && key <= std::string_view(last_)) { throw std::invalid_argument("termdict: keys must be strictly increasing"); }
		if (pending_ == 0) { first_key_.assign(key); }
		std::size_t shared = 0;
		if (pending_ % restart_interval != 0) {
			while (shared < key.size() && shared < last_.size() && key[shared] == last_[shared]) { ++shared; }
		} else {
			restarts_.push_back(entries_.size());
		}
		put(entries_, shared);
		put(entries_, key.size() - shared);
		entries_.append(key.substr(shared));
		put(entries_, value.size());
		entries_.append(value);
		++pending_;
		last_.assign(key);
		++count_;
		if (pending_ >= max_block_entries || (pending_ >= min_block_entries && entries_.size() >= target_block_bytes)) { cut(); }
	}

	tables finish() {
		if (pending_) { cut(); }
		out_.index = index_.finish();
		return std::move(out_);
	}

	std::uint64_t size() const noexcept { return count_; }

private:
	static void put(std::string& out, std::uint64_t v) {
		char buf[varint::leb128::max_bytes];
		out.append(buf, varint::leb128::encode(buf, v));
	}

	void cut() {
		std::string head;
		put(head, pending_);
		put(head, restarts_.size());
		for (auto r : restarts_) { put(head, r); }
		const std::uint64_t offset = out_.blocks.size();
		put(out_.blocks, head.size() + entries_.size());
		out_.blocks += head;
		out_.blocks += entries_;
		index_.insert(first_key_, offset);
		entries_.clear();
		restarts_.clear();
		pending_ = 0;
	}

	tables out_;
	fst::builder index_;
	std::string entries_, last_, first_key_;
	std::vector<std::uint64_t> restarts_;
	std::size_t pending_ = 0;  // entries in the open block
	std::uint64_t count_ = 0;
};

// A decoded view of one block, bounds-checked against the block's bytes.
class block {
public:
	block(std::string_view bytes, std::uint64_t offset) {
		const char* p = bytes.data() + offset;
		const char* end = bytes.data() + bytes.size();
		if (offset >= bytes.size()) { throw corrupt(); }
		std::uint64_t length = 0;
		if (!varint::leb128::decode(&p, end, length) || length > std::uint64_t(end - p)) { throw corrupt(); }
		end_ = p + length;
		std::uint64_t count = 0, restarts = 0;
		if (!varint::leb128::decode(&p, end_, count) || !varint::leb128::decode(&p, end_, restarts)) { throw corrupt(); }
		if (count == 0 || count > max_block_entries || restarts != (count + restart_interval - 1) / restart_interval) { throw corrupt(); }
		restart_count_ = std::size_t(restarts);
		for (std::size_t i = 0; i < restart_count_; ++i) {
			if (!varint::leb128::decode(&p, end_, restarts_[i])) { throw corrupt(); }
		}
		entries_ = p;
		if (restarts_[0] != 0) { throw corrupt(); }
		for (std::size_t i = 1; i < restart_count_; ++i) {
			if (restarts_[i] <= restarts_[i - 1] || restarts_[i] >= std::uint64_t(end_ - entries_)) { throw corrupt(); }
		}
		count_ = count;
		next_block_ = std::uint64_t(end_ - bytes.data());
	}

	std::size_t count() const noexcept { return count_; }
	std::uint64_t next_block() const noexcept { return next_block_; }

	// Sequential decoding from a restart point; key accumulates the prefix.
	class cursor {
	public:
		cursor(const block& b, std::size_t restart) : b_(&b), p_(b.entries_ + b.restarts_[restart]), index_(restart * restart_interval) {}

		// Decodes the next entry; false at the block's end.
		bool next() {
			if (index_ == b_->count_) { return false; }
			std::uint64_t shared = 0, suffix = 0, value = 0;
			if (!varint::leb128::decode(&p_, b_->end_, shared) || !varint::leb128::decode(&p_, b_->end_, suffix)) { throw corrupt(); }
			if (shared > key_.size() || suffix > std::uint64_t(b_->end_ - p_)) { throw corrupt(); }
			if (index_ % restart_interval == 0 && shared != 0) { throw corrupt(); }
			key_.resize(shared);
			key_.append(p_, suffix);
			p_ += suffix;
			if (!varint::leb128::decode(&p_, b_->end_, value) || value > std::uint64_t(b_->end_ - p_)) { throw corrupt(); }
			value_ = std::string_view(p_, value);
			p_ += value;
			++index_;
			return true;
		}

		const std::string& key() const noexcept { return key_; }
		std::string_view value() const noexcept { return value_; }

	private:
		const block* b_;
		const char* p_;
		std::size_t index_;
		std::string key_;
		std::string_view value_;
	};

	// The full key at restart point r (restart entries are not prefix-compressed).
	std::string_view restart_key(std::size_t r) const {
		const char* p = entries_ + restarts_[r];
		std::uint64_t shared = 0, suffix = 0;
		if (!varint::leb128::decode(&p, end_, shared) || !varint::leb128::decode(&p, end_, suffix) || shared != 0 ||
			suffix > std::uint64_t(end_ - p)) {
			throw corrupt();
		}
		return {p, suffix};
	}

	std::size_t restarts() const noexcept { return restart_count_; }

	// Finds key's value by scanning from restart r without building keys: track how much
	// of `key` the previous entry matched (m). An entry sharing more than m with its
	// predecessor sorts like the predecessor (below key); one sharing less sorts above.
	std::optional<std::string_view> find_from(std::size_t r, std::string_view key) const {
		const char* p = entries_ + restarts_[r];
		std::size_t m = 0;
		const std::size_t last = std::min(count_, (r + 1) * restart_interval);
		for (std::size_t i = r * restart_interval; i < last; ++i) {
			std::uint64_t shared = 0, suffix = 0, value = 0;
			if (!varint::leb128::decode(&p, end_, shared) || !varint::leb128::decode(&p, end_, suffix) || suffix > std::uint64_t(end_ - p)) {
				throw corrupt();
			}
			const char* sfx = p;
			p += suffix;
			if (!varint::leb128::decode(&p, end_, value) || value > std::uint64_t(end_ - p)) { throw corrupt(); }
			const char* val = p;
			p += value;
			if (i == r * restart_interval) {
				if (shared != 0) { throw corrupt(); }
			} else if (shared > m) {
				continue;  // still below key
			} else if (shared < m) {
				return std::nullopt;  // past key
			}
			// shared == m: compare the suffix with key[m..].
			std::size_t n = std::min<std::size_t>(suffix, key.size() - m);
			std::size_t j = 0;
			while (j < n && sfx[j] == key[m + j]) { ++j; }
			if (j == suffix && m + j == key.size()) { return std::string_view(val, value); }
			if (j < n ? std::uint8_t(sfx[j]) > std::uint8_t(key[m + j]) : m + j == key.size()) { return std::nullopt; }
			m += j;
		}
		return std::nullopt;
	}

private:
	const char* entries_ = nullptr;
	const char* end_ = nullptr;
	std::uint64_t restarts_[(max_block_entries + restart_interval - 1) / restart_interval] = {};
	std::size_t restart_count_ = 0;
	std::size_t count_ = 0;
	std::uint64_t next_block_ = 0;
};

// A read-only dictionary over bytes owned elsewhere. The FST index is validated at
// construction; blocks are bounds-checked as they are decoded.
class reader {
public:
	reader(std::string_view blocks, std::string_view index) : blocks_(blocks), index_(index) {}

	// The value stored for key, if present.
	std::optional<std::string_view> find(std::string_view key) const {
		auto offset = index_.floor_value(key);
		if (!offset) { return std::nullopt; }
		block b(blocks_, *offset);
		// The last restart whose key is <= key.
		std::size_t lo = 0, hi = b.restarts();
		while (hi - lo > 1) {
			std::size_t mid = (lo + hi) / 2;
			if (b.restart_key(mid) <= key) { lo = mid; } else { hi = mid; }
		}
		return b.find_from(lo, key);
	}

	// Calls f(key, value) for every entry with key >= from, in order, until f returns false.
	template <typename F>
	void scan(std::string_view from, F&& f) const {
		if (blocks_.empty()) { return; }
		std::uint64_t offset = index_.floor_value(from).value_or(0);
		while (offset < blocks_.size()) {
			block b(blocks_, offset);
			block::cursor c(b, 0);
			while (c.next()) {
				if (std::string_view(c.key()) < from) { continue; }
				if (!f(std::string_view(c.key()), c.value())) { return; }
			}
			offset = b.next_block();
		}
	}

private:
	std::string_view blocks_;
	fst::map index_;
};

}  // namespace termdict
