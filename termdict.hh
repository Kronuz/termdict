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
// Key suffixes are coded over the dictionary's alphabet (the bytes its keys use): each
// byte becomes its rank, bit_width(size - 1) bits wide, so codes sort as the bytes do.
// The full byte range is the alphabet when none is given (8-bit codes, the bytes).
//
// Index: [alphabet size (0: all 256)][alphabet bytes, increasing][FST over first keys]
// Block (integers LEB128): [block length][entry count][restart count][restart offsets...]
// then one run per restart point: [suffix bits length][suffix bits][entries...]
// Entry: [lengths byte: suffix length << 4 | value length, each 15 when it continues as a
// LEB128 of the rest][shared prefix length][suffix rest?][value rest?][value]
// Suffix lengths count symbols; a run's suffixes are one little-endian bit stream. Restart
// entries have a shared length of 0; restart offsets are relative to the first run.

#pragma once

#include "fst.hh"
#include "varint.hh"

#include <algorithm>
#include <array>
#include <bit>
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

inline constexpr std::size_t restart_interval = 32;
inline constexpr std::size_t min_block_entries = 32;
inline constexpr std::size_t max_block_entries = 64;
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

// Builds from keys in strictly increasing byte order. `alphabet` holds every byte the
// keys use, in any order (empty: all 256); a key with another byte is refused.
class builder {
public:
	explicit builder(std::string_view alphabet = {}) {
		std::array<bool, 256> used{};
		for (char c : alphabet) { used[std::uint8_t(c)] = true; }
		for (unsigned c = 0; c < 256; ++c) {
			if (used[c] || alphabet.empty()) { alphabet_.push_back(char(c)); }
		}
		code_.fill(-1);
		for (std::size_t i = 0; i < alphabet_.size(); ++i) { code_[std::uint8_t(alphabet_[i])] = std::int16_t(i); }
		width_ = alphabet_.size() <= 1 ? 1u : unsigned(std::bit_width(alphabet_.size() - 1));
	}

	void add(std::string_view key, std::string_view value) {
		if (count_ && key <= std::string_view(last_)) { throw std::invalid_argument("termdict: keys must be strictly increasing"); }
		for (char c : key) {
			if (code_[std::uint8_t(c)] < 0) { throw std::invalid_argument("termdict: key byte outside the alphabet"); }
		}
		if (pending_ == 0) { first_key_.assign(key); }
		std::size_t shared = 0;
		if (pending_ % restart_interval != 0) {
			while (shared < key.size() && shared < last_.size() && key[shared] == last_[shared]) { ++shared; }
		} else {
			end_run();
			restarts_.push_back(entries_.size());
		}
		const std::size_t suffix = key.size() - shared;
		run_.push_back(char(std::min<std::size_t>(suffix, 15) << 4 | std::min<std::size_t>(value.size(), 15)));
		put(run_, shared);
		if (suffix >= 15) { put(run_, suffix - 15); }
		if (value.size() >= 15) { put(run_, value.size() - 15); }
		run_.append(value);
		for (char c : key.substr(shared)) {
			bits_ |= std::uint64_t(code_[std::uint8_t(c)]) << nbits_;
			nbits_ += width_;
			while (nbits_ >= 8) {
				run_bits_.push_back(char(bits_));
				bits_ >>= 8;
				nbits_ -= 8;
			}
		}
		++pending_;
		last_.assign(key);
		++count_;
		if (pending_ >= max_block_entries || (pending_ >= min_block_entries && entries_.size() + run_bits_.size() + run_.size() >= target_block_bytes)) { cut(); }
	}

	tables finish() {
		if (pending_) { cut(); }
		put(out_.index, alphabet_.size() == 256 ? 0 : alphabet_.size());
		if (alphabet_.size() != 256) { out_.index += alphabet_; }
		out_.index += index_.finish();
		return std::move(out_);
	}

	std::uint64_t size() const noexcept { return count_; }

	// Whether the next add() starts a restart run (a block's first entry, or every
	// restart_interval-th). A value can then hold state that the entries after it in the
	// same run encode against, which find_run() replays.
	bool next_is_restart() const noexcept { return pending_ % restart_interval == 0; }

private:
	static void put(std::string& out, std::uint64_t v) {
		char buf[varint::leb128::max_bytes];
		out.append(buf, varint::leb128::encode(buf, v));
	}

	// Closes the open run: its suffix bits, then its entries.
	void end_run() {
		if (nbits_) { run_bits_.push_back(char(bits_)); }
		if (run_.empty()) { return; }
		put(entries_, run_bits_.size());
		entries_ += run_bits_;
		entries_ += run_;
		run_.clear();
		run_bits_.clear();
		bits_ = 0;
		nbits_ = 0;
	}

	void cut() {
		end_run();
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
	std::string alphabet_;
	std::array<std::int16_t, 256> code_{};
	unsigned width_ = 8;
	std::string run_, run_bits_;  // the open run's entries and suffix bits
	std::uint64_t bits_ = 0;
	unsigned nbits_ = 0;
	std::string entries_, last_, first_key_;
	std::vector<std::uint64_t> restarts_;
	std::size_t pending_ = 0;  // entries in the open block
	std::uint64_t count_ = 0;
};

// The symbols of a dictionary's alphabet, by code.
struct alphabet {
	std::array<char, 256> symbol{};
	unsigned size = 256, width = 8;

	// Reads the alphabet at the start of the index bytes; returns the FST's bytes.
	std::string_view parse(std::string_view index) {
		const char* p = index.data();
		const char* end = p + index.size();
		std::uint64_t n = 0;
		if (index.empty()) {
			n = 0;
		} else if (!varint::leb128::decode(&p, end, n) || n > 255 || n > std::uint64_t(end - p)) {
			throw corrupt();
		}
		size = n ? unsigned(n) : 256;
		for (unsigned i = 0; i < size; ++i) {
			symbol[i] = n ? p[i] : char(i);
			if (i && std::uint8_t(symbol[i]) <= std::uint8_t(symbol[i - 1])) { throw corrupt(); }
		}
		width = size <= 1 ? 1u : unsigned(std::bit_width(size - 1));
		return {p + n, std::size_t(end - p - std::ptrdiff_t(n))};
	}
};

// A decoded view of one block, bounds-checked against the block's bytes.
class block {
public:
	block(std::string_view bytes, std::uint64_t offset, const alphabet& a) : a_(&a) {
		if (offset >= bytes.size()) { throw corrupt(); }  // before forming the pointer
		const char* p = bytes.data() + offset;
		const char* end = bytes.data() + bytes.size();
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

	// Reads an entry's header at p: the lengths byte, the shared prefix length, and any
	// continued lengths; leaves p at the value, bounds-checked.
	static void header(const char*& p, const char* end, std::uint64_t& shared, std::uint64_t& suffix, std::uint64_t& value) {
		if (p == end) { throw corrupt(); }
		const std::uint8_t lengths = std::uint8_t(*p++);
		suffix = lengths >> 4;
		value = lengths & 15;
		std::uint64_t more = 0;
		if (!varint::leb128::decode(&p, end, shared)) { throw corrupt(); }
		if (suffix == 15) {
			if (!varint::leb128::decode(&p, end, more) || more > 1u << 30) { throw corrupt(); }
			suffix += more;
		}
		if (value == 15) {
			if (!varint::leb128::decode(&p, end, more) || more > std::uint64_t(end - p)) { throw corrupt(); }
			value += more;
		}
		if (value > std::uint64_t(end - p)) { throw corrupt(); }
	}

	// A run's suffix bits, read in order. Symbols load 8 bytes at a time while the block's
	// bytes allow (bits past the run's are masked off), byte by byte at its end.
	class suffixes {
	public:
		suffixes() = default;
		suffixes(const alphabet& a, const char* bits, std::uint64_t bytes, const char* end)
			: a_(&a), bits_(bits), end_(end), size_(bytes * 8), mask_((1u << a.width) - 1), width_(a.width) {}
		// Starts the next suffix of n symbols; throws when the run's bits do not hold it.
		void start(std::uint64_t n) {
			if (n * width_ > size_ - at_) { throw corrupt(); }  // n < 2^31 (header())
		}
		// Decodes the next n symbols into out (after start() has checked their bits): as many
		// as one 64-bit load holds at a time.
		void decode(std::uint64_t n, char* out) {
			const unsigned per = 57 / width_;
			unsigned bad = 0;
			while (n) {
				const char* p = bits_ + (at_ >> 3);
				std::uint64_t v = 0;
				if (end_ - p >= 8) [[likely]] {
					std::memcpy(&v, p, 8);
					if constexpr (std::endian::native == std::endian::big) { v = __builtin_bswap64(v); }
				} else {
					for (std::ptrdiff_t i = 0; i < end_ - p; ++i) { v |= std::uint64_t(std::uint8_t(p[i])) << (8 * i); }
				}
				v >>= at_ & 7;
				const unsigned k = unsigned(std::min<std::uint64_t>(n, per));
				for (unsigned j = 0; j < k; ++j) {
					const unsigned code = unsigned(v) & mask_;
					v >>= width_;
					bad |= unsigned(code >= a_->size);
					*out++ = a_->symbol[code];
				}
				at_ += std::uint64_t(k) * width_;
				n -= k;
			}
			if (bad) [[unlikely]] { throw corrupt(); }
		}
		void skip(std::uint64_t n) { at_ += n * width_; }
		// The next symbol, for comparisons that stop at the first difference.
		char next() {
			const char* p = bits_ + (at_ >> 3);
			std::uint64_t v = 0;
			if (end_ - p >= 8) [[likely]] {
				std::memcpy(&v, p, 8);
				if constexpr (std::endian::native == std::endian::big) { v = __builtin_bswap64(v); }
			} else {
				for (std::ptrdiff_t i = 0; i < end_ - p; ++i) { v |= std::uint64_t(std::uint8_t(p[i])) << (8 * i); }
			}
			const unsigned code = unsigned(v >> (at_ & 7)) & mask_;
			at_ += width_;
			if (code >= a_->size) [[unlikely]] { throw corrupt(); }
			return a_->symbol[code];
		}

	private:
		const alphabet* a_ = nullptr;
		const char* bits_ = nullptr;
		const char* end_ = nullptr;
		std::uint64_t size_ = 0, at_ = 0;
		unsigned mask_ = 0, width_ = 8;
	};

	// Opens run r: its suffix bits, and p at its first entry.
	suffixes run(std::size_t r, const char*& p) const {
		p = entries_ + restarts_[r];
		std::uint64_t n = 0;
		if (!varint::leb128::decode(&p, end_, n) || n > std::uint64_t(end_ - p)) { throw corrupt(); }
		suffixes s(*a_, p, n, end_);
		p += n;
		return s;
	}

	// Sequential decoding from a restart point; key accumulates the prefix.
	class cursor {
	public:
		cursor(const block& b, std::size_t restart) : b_(&b), index_(restart * restart_interval) {}

		// Decodes the next entry; false at the block's end.
		bool next() {
			if (index_ == b_->count_) { return false; }
			if (index_ % restart_interval == 0) { s_ = b_->run(index_ / restart_interval, p_); }
			std::uint64_t shared = 0, suffix = 0, value = 0;
			header(p_, b_->end_, shared, suffix, value);
			if (shared > key_.size()) { throw corrupt(); }
			if (index_ % restart_interval == 0 && shared != 0) { throw corrupt(); }
			s_.start(suffix);
			key_.resize(shared + suffix);
			s_.decode(suffix, key_.data() + shared);
			value_ = std::string_view(p_, value);
			p_ += value;
			++index_;
			return true;
		}

		const std::string& key() const noexcept { return key_; }
		std::string_view value() const noexcept { return value_; }

	private:
		const block* b_;
		const char* p_ = nullptr;
		suffixes s_;
		std::size_t index_;
		std::string key_;
		std::string_view value_;
	};

	// The full key at restart point r against key: negative, zero, or positive as it sorts
	// before, equal to, or after key.
	int compare_restart(std::size_t r, std::string_view key) const {
		const char* p = nullptr;
		suffixes s = run(r, p);
		std::uint64_t shared = 0, suffix = 0, value = 0;
		header(p, end_, shared, suffix, value);
		if (shared != 0) { throw corrupt(); }
		s.start(suffix);
		for (std::uint64_t i = 0; i < suffix; ++i) {
			if (i == key.size()) { return 1; }
			const auto c = std::uint8_t(s.next()), k = std::uint8_t(key[i]);
			if (c != k) { return c < k ? -1 : 1; }
		}
		return suffix == key.size() ? 0 : -1;
	}

	std::size_t restarts() const noexcept { return restart_count_; }

	// Finds key's value by scanning from restart r without building keys: track how much
	// of `key` the previous entry matched (m). An entry sharing more than m with its
	// predecessor sorts like the predecessor (below key); one sharing less sorts above.
	// visit(value) sees each entry's value from the restart through key's, in order.
	template <typename Visit>
	std::optional<std::string_view> find_from(std::size_t r, std::string_view key, Visit&& visit) const {
		const char* p = nullptr;
		suffixes sfx = run(r, p);
		std::size_t m = 0;
		const std::size_t last = std::min(count_, (r + 1) * restart_interval);
		for (std::size_t i = r * restart_interval; i < last; ++i) {
			std::uint64_t shared = 0, suffix = 0, value = 0;
			header(p, end_, shared, suffix, value);
			sfx.start(suffix);
			const char* val = p;
			p += value;
			visit(std::string_view(val, value));
			if (i == r * restart_interval) {
				if (shared != 0) { throw corrupt(); }
			} else if (shared > m) {
				sfx.skip(suffix);
				continue;  // still below key
			} else if (shared < m) {
				return std::nullopt;  // past key
			}
			// shared == m: compare the suffix with key[m..], a chunk at a time.
			const std::size_t n = std::min<std::size_t>(suffix, key.size() - m);
			std::size_t j = 0;
			char c = 0;
			for (; j < n; ++j) {
				c = sfx.next();
				if (c != key[m + j]) { break; }
			}
			if (j == suffix && m + j == key.size()) { return std::string_view(val, value); }
			if (j < n ? std::uint8_t(c) > std::uint8_t(key[m + j]) : m + j == key.size()) { return std::nullopt; }
			sfx.skip(suffix - j - (j < n ? 1 : 0));
			m += j;
		}
		return std::nullopt;
	}

private:
	const alphabet* a_;
	const char* entries_ = nullptr;
	const char* end_ = nullptr;
	std::uint64_t restarts_[(max_block_entries + restart_interval - 1) / restart_interval] = {};
	std::size_t restart_count_ = 0;
	std::size_t count_ = 0;
	std::uint64_t next_block_ = 0;
};

// A read-only dictionary over bytes owned elsewhere. The alphabet and FST index are
// validated at construction; blocks are bounds-checked as they are decoded.
class reader {
public:
	// Called with a byte range of `blocks` before any of it is read; returning false
	// rejects the range (the read throws corrupt). For checksum verification on first use.
	using verifier = bool (*)(void* context, std::uint64_t offset, std::uint64_t length);

	reader(std::string_view blocks, std::string_view index) : blocks_(blocks), index_(alphabet_.parse(index)) {}
	reader(std::string_view blocks, std::string_view index, verifier verify, void* context)
		: blocks_(blocks), index_(alphabet_.parse(index)), verify_(verify), context_(context) {}

	// The bytes keys use (all 256 when the dictionary names none).
	std::string_view alphabet_bytes() const noexcept { return {alphabet_.symbol.data(), alphabet_.size}; }

	// The value stored for key, if present.
	std::optional<std::string_view> find(std::string_view key) const {
		return find_run(key, [](std::string_view) {});
	}

	// The same; visit(value) also sees every value of key's restart run up to and including
	// key's own (and some past where key would sort when it is absent).
	template <typename Visit>
	std::optional<std::string_view> find_run(std::string_view key, Visit&& visit) const {
		auto offset = index_.floor_value(key);
		if (!offset) { return std::nullopt; }
		block b = open(*offset);
		// The last restart whose key is <= key.
		std::size_t lo = 0, hi = b.restarts();
		while (hi - lo > 1) {
			std::size_t mid = (lo + hi) / 2;
			if (b.compare_restart(mid, key) <= 0) { lo = mid; } else { hi = mid; }
		}
		return b.find_from(lo, key, visit);
	}

	// Calls f(key, value) for every entry with key >= from, in order, until f returns false.
	template <typename F>
	void scan(std::string_view from, F&& f) const {
		if (blocks_.empty()) { return; }
		std::uint64_t offset = index_.floor_value(from).value_or(0);
		while (offset < blocks_.size()) {
			block b = open(offset);
			block::cursor c(b, 0);
			while (c.next()) {
				if (std::string_view(c.key()) < from) { continue; }
				if (!f(std::string_view(c.key()), c.value())) { return; }
			}
			offset = b.next_block();
		}
	}

private:
	// The block at offset, verified first when a verifier is set: its length prefix, then
	// the whole block.
	block open(std::uint64_t offset) const {
		if (verify_ && offset < blocks_.size()) {
			const std::uint64_t head = std::min<std::uint64_t>(varint::leb128::max_bytes, blocks_.size() - offset);
			if (!verify_(context_, offset, head)) { throw corrupt(); }
			const char* p = blocks_.data() + offset;
			std::uint64_t length = 0;
			if (!varint::leb128::decode(&p, blocks_.data() + offset + head, length) || length > std::uint64_t(blocks_.data() + blocks_.size() - p)) { throw corrupt(); }
			if (!verify_(context_, offset, std::uint64_t(p - (blocks_.data() + offset)) + length)) { throw corrupt(); }
		}
		return block(blocks_, offset, alphabet_);
	}

	termdict::alphabet alphabet_;
	std::string_view blocks_;
	fst::map index_;
	verifier verify_ = nullptr;
	void* context_ = nullptr;
};

}  // namespace termdict
