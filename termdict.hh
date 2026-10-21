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
// The longest key: a block's first key is an index (fst) key.
inline constexpr std::size_t max_key_bytes = fst::max_key_length;

// Thrown when the bytes are not a well-formed dictionary; callers fence the data.
struct corrupt : std::runtime_error {
	corrupt() : std::runtime_error("termdict: corrupt block") {}
};

namespace detail {

// Out of line and cold, so the decoders' many checks leave their hot paths inlinable.
[[noreturn, gnu::cold, gnu::noinline]] inline void fail() { throw corrupt(); }

// A LEB128 at p, the one-byte case inline.
inline bool leb(const char*& p, const char* end, std::uint64_t& v) noexcept {
	if (p != end && !(std::uint8_t(*p) & 0x80)) [[likely]] {
		v = std::uint8_t(*p++);
		return true;
	}
	return varint::leb128::decode(&p, end, v);
}

}  // namespace detail

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
		if (key.size() > max_key_bytes) { throw std::invalid_argument("termdict: key longer than max_key_bytes"); }
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
			detail::fail();
		}
		size = n ? unsigned(n) : 256;
		for (unsigned i = 0; i < size; ++i) {
			symbol[i] = n ? p[i] : char(i);
			if (i && std::uint8_t(symbol[i]) <= std::uint8_t(symbol[i - 1])) { detail::fail(); }
		}
		width = size <= 1 ? 1u : unsigned(std::bit_width(size - 1));
		return {p + n, std::size_t(end - p - std::ptrdiff_t(n))};
	}
};

// A decoded view of one block, bounds-checked against the block's bytes.
class block {
public:
	block(std::string_view bytes, std::uint64_t offset, const alphabet& a) : a_(&a) {
		if (offset >= bytes.size()) { detail::fail(); }  // before forming the pointer
		const char* p = bytes.data() + offset;
		const char* end = bytes.data() + bytes.size();
		std::uint64_t length = 0;
		if (!varint::leb128::decode(&p, end, length) || length > std::uint64_t(end - p)) { detail::fail(); }
		end_ = p + length;
		std::uint64_t count = 0, restarts = 0;
		if (!varint::leb128::decode(&p, end_, count) || !varint::leb128::decode(&p, end_, restarts)) { detail::fail(); }
		if (count == 0 || count > max_block_entries || restarts != (count + restart_interval - 1) / restart_interval) { detail::fail(); }
		restart_count_ = std::size_t(restarts);
		for (std::size_t i = 0; i < restart_count_; ++i) {
			if (!varint::leb128::decode(&p, end_, restarts_[i])) { detail::fail(); }
		}
		entries_ = p;
		if (restarts_[0] != 0) { detail::fail(); }
		for (std::size_t i = 1; i < restart_count_; ++i) {
			if (restarts_[i] <= restarts_[i - 1] || restarts_[i] >= std::uint64_t(end_ - entries_)) { detail::fail(); }
		}
		count_ = count;
		next_block_ = std::uint64_t(end_ - bytes.data());
	}

	std::size_t count() const noexcept { return count_; }
	std::uint64_t next_block() const noexcept { return next_block_; }

	// Reads an entry's header at p: the lengths byte, the shared prefix length, and any
	// continued lengths; leaves p at the value, bounds-checked.
	[[gnu::always_inline]] static void header(const char*& p, const char* end, std::uint64_t& shared, std::uint64_t& suffix, std::uint64_t& value) {
		if (p == end) { detail::fail(); }
		const std::uint8_t lengths = std::uint8_t(*p++);
		suffix = lengths >> 4;
		value = lengths & 15;
		if (!detail::leb(p, end, shared)) { detail::fail(); }
		std::uint64_t more = 0;
		if (suffix == 15) [[unlikely]] {
			if (!detail::leb(p, end, more) || more > max_key_bytes - 15) { detail::fail(); }
			suffix += more;
		}
		if (value == 15) [[unlikely]] {
			if (!detail::leb(p, end, more) || more > std::uint64_t(end - p)) { detail::fail(); }
			value += more;
		}
		if (value > std::uint64_t(end - p)) { detail::fail(); }
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
			if (n * width_ > size_ - at_) { detail::fail(); }  // n < 2^32 (header())
		}
		// Decodes the next n symbols into out (after start() has checked their bits).
		[[gnu::always_inline]] void decode(std::uint64_t n, char* out) { dispatch<false>(n, out); }
		void skip(std::uint64_t n) { at_ += n * width_; }
		// The next symbol, for comparisons that stop at the first difference.
		char next() {
			const unsigned code = unsigned(load(bits_ + (at_ >> 3), end_) >> (at_ & 7)) & mask_;
			at_ += width_;
			if (code >= a_->size) [[unlikely]] { detail::fail(); }
			return a_->symbol[code];
		}

	private:
		friend class block;

		// Bytes decode_padded() may write past the suffix.
		static constexpr std::size_t padding = 16;

		// The same, writing whole chunks of symbols past the suffix's end (at most padding
		// bytes), so no branch depends on the suffix's length.
		[[gnu::always_inline]] void decode_padded(std::uint64_t n, char* out) { dispatch<true>(n, out); }

		// 8 bytes at p, or what the block holds of them; little-endian.
		static std::uint64_t load(const char* p, const char* end) noexcept {
			std::uint64_t v = 0;
			if (end - p >= 8) [[likely]] {
				std::memcpy(&v, p, 8);
				if constexpr (std::endian::native == std::endian::big) { v = __builtin_bswap64(v); }
			} else {
				for (std::ptrdiff_t i = 0; i < end - p; ++i) { v |= std::uint64_t(std::uint8_t(p[i])) << (8 * i); }
			}
			return v;
		}

		// The width as a constant, so shifts are immediates and chunks unroll.
		template <bool Padded>
		[[gnu::always_inline]] void dispatch(std::uint64_t n, char* out) {
			switch (width_) {
			case 1: return decode<1, Padded>(n, out);
			case 2: return decode<2, Padded>(n, out);
			case 3: return decode<3, Padded>(n, out);
			case 4: return decode<4, Padded>(n, out);
			case 5: return decode<5, Padded>(n, out);
			case 6: return decode<6, Padded>(n, out);
			case 7: return decode<7, Padded>(n, out);
			default: return decode<8, Padded>(n, out);
			}
		}

		// Chunks of `per` symbols from one 64-bit load each (57 bits remain after the shift
		// to the bit offset). Works on locals: out's char stores may alias any member.
		template <unsigned W, bool Padded>
		[[gnu::always_inline]] void decode(std::uint64_t n, char* out) {
			constexpr unsigned per = std::min(57 / W, unsigned(padding));
			constexpr std::uint64_t mask = (1u << W) - 1;
			// A 1 at the bottom of each lane, and at the bottom of the lane above each.
			constexpr std::uint64_t ones = [] {
				std::uint64_t v = 0;
				for (unsigned j = 0; j < per; ++j) { v |= std::uint64_t(1) << (j * W); }
				return v;
			}();
			constexpr std::uint64_t carries = ones << W;
			std::uint64_t at = at_;
			at_ += n * W;
			if constexpr (W == 8) {
				if (a_->size == 256) {  // codes are the bytes, whole bytes apart
					const char* const from = bits_ + (at >> 3);
					if constexpr (Padded) {
						if (n <= padding && end_ - from >= std::ptrdiff_t(padding)) {
							std::memcpy(out, from, padding);  // a fixed size: two moves, no call
							return;
						}
					}
					std::memcpy(out, from, n);
					return;
				}
			}
			const char* const bits = bits_;
			const char* const end = end_;
			const char* const symbol = a_->symbol.data();
			// Adding 2^W - size to every lane carries out of a lane whose code is >= size; the
			// lowest such carry is a real one (nothing carries into it), and valid codes never
			// carry.
			const std::uint64_t lift = std::uint64_t((1u << W) - a_->size) * ones;
			std::uint64_t seen = 0;
			while (n) {
				const unsigned k = unsigned(std::min<std::uint64_t>(n, per));
				// Codes past the suffix read as 0, which is always valid.
				const std::uint64_t v = (load(bits + (at >> 3), end) >> (at & 7)) & ((std::uint64_t(1) << (k * W)) - 1);
				seen |= ((v + lift) ^ v ^ lift) & carries;
				if constexpr (Padded) {
					for (unsigned j = 0; j < per; ++j) { out[j] = symbol[(v >> (j * W)) & mask]; }
				} else {
					for (unsigned j = 0; j < k; ++j) { out[j] = symbol[(v >> (j * W)) & mask]; }
				}
				out += k;
				at += std::uint64_t(k) * W;
				n -= k;
			}
			if (seen) [[unlikely]] { detail::fail(); }
		}

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
		if (!varint::leb128::decode(&p, end_, n) || n > std::uint64_t(end_ - p)) { detail::fail(); }
		suffixes s(*a_, p, n, end_);
		p += n;
		return s;
	}

private:
	friend class reader;

	// Where a sequential decode stands: the next entry's index, its bytes, its run's bits.
	struct position {
		std::size_t index = 0;
		const char* p = nullptr;
		suffixes s;
	};

	// Decodes the entry at `at` and moves past it. Its suffix lands in key after its shared
	// prefix (key's first `prev` bytes hold the previous key), key grown to fit and never
	// shrunk (with suffixes::padding bytes past the key when Padded); returns the key's length.
	template <bool Padded, class Buffer>
	std::size_t step(position& at, Buffer& key, std::size_t prev, std::string_view& value) const {
		const bool restart = at.index % restart_interval == 0;
		if (restart) { at.s = run(at.index / restart_interval, at.p); }
		const char* p = at.p;
		std::uint64_t shared = 0, suffix = 0, length = 0;
		header(p, end_, shared, suffix, length);
		if (shared > (restart ? 0 : prev)) { detail::fail(); }
		at.s.start(suffix);
		const std::size_t size = std::size_t(shared + suffix);
		if (size + (Padded ? suffixes::padding : 0) > key.size()) [[unlikely]] { key.resize(size + (Padded ? suffixes::padding : 0)); }
		if constexpr (Padded) {
			at.s.decode_padded(suffix, key.data() + shared);
		} else {
			at.s.decode(suffix, key.data() + shared);
		}
		value = std::string_view(p, std::size_t(length));
		at.p = p + length;
		++at.index;
		return size;
	}

public:
	// Sequential decoding from a restart point; key accumulates the prefix.
	class cursor {
	public:
		cursor(const block& b, std::size_t restart) : b_(&b) { at_.index = restart * restart_interval; }

		// Decodes the next entry; false at the block's end.
		bool next() {
			if (at_.index == b_->count_) { return false; }
			key_.resize(b_->step<false>(at_, key_, key_.size(), value_));
			return true;
		}

		const std::string& key() const noexcept { return key_; }
		std::string_view value() const noexcept { return value_; }

	private:
		const block* b_;
		position at_;
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
		if (shared != 0) { detail::fail(); }
		s.start(suffix);
		for (std::uint64_t i = 0; i < suffix; ++i) {
			if (i == key.size()) { return 1; }
			const auto c = std::uint8_t(s.next()), k = std::uint8_t(key[i]);
			if (c != k) { return c < k ? -1 : 1; }
		}
		return suffix == key.size() ? 0 : -1;
	}

	std::size_t restarts() const noexcept { return restart_count_; }

private:
	// The first entry of a run walk at or above its key, and what decoding it needs.
	struct landing {
		std::size_t index = 0;
		std::uint64_t shared = 0, suffix = 0;
		std::string_view value;
		suffixes s;                // at the entry's suffix
		const char* p = nullptr;   // past the entry
		bool equal = false;        // the entry's key is the key
	};

	// Walks run r to its first entry >= key without building keys: track how much of key
	// the previous entry matched (m). An entry sharing more than m with its predecessor
	// sorts like the predecessor (below key); one sharing less sorts above. visit(value)
	// sees each entry's value from the restart through the landing's, in order. False when
	// every entry of the run sorts below key.
	template <typename Visit>
	bool walk_to(std::size_t r, std::string_view key, Visit&& visit, landing& at) const {
		const char* p = nullptr;
		suffixes sfx = run(r, p);
		std::size_t m = 0;
		const std::size_t first = r * restart_interval, last = std::min(count_, first + restart_interval);
		for (std::size_t i = first; i < last; ++i) {
			std::uint64_t shared = 0, suffix = 0, value = 0;
			header(p, end_, shared, suffix, value);
			sfx.start(suffix);
			const std::string_view val(p, std::size_t(value));
			p += value;
			visit(val);
			const suffixes here = sfx;
			bool equal = false;
			if (i == first && shared != 0) { detail::fail(); }
			if (i != first && shared > m) {
				sfx.skip(suffix);
				continue;  // still below key
			}
			if (i == first || shared == m) {
				// Compare the suffix with key[m..], a symbol at a time.
				const std::size_t n = std::min<std::size_t>(suffix, key.size() - m);
				std::size_t j = 0;
				char c = 0;
				for (; j < n; ++j) {
					c = sfx.next();
					if (c != key[m + j]) { break; }
				}
				equal = j == suffix && m + j == key.size();
				if (!equal && !(j < n ? std::uint8_t(c) > std::uint8_t(key[m + j]) : m + j == key.size())) {
					sfx.skip(suffix - j - (j < n ? 1 : 0));
					m += j;
					continue;  // below key
				}
			}  // else shared < m: past key
			at = {i, shared, suffix, val, here, p, equal};
			return true;
		}
		return false;
	}

	// Builds a landing's key in key (padded), its first `shared` bytes from's; at moves past
	// it. Returns the key's length.
	template <class Buffer>
	std::size_t land(landing l, std::string_view from, Buffer& key, position& at) const {
		const std::size_t size = std::size_t(l.shared + l.suffix);
		if (size + suffixes::padding > key.size()) { key.resize(size + suffixes::padding); }
		from.copy(key.data(), std::size_t(l.shared));
		l.s.decode_padded(l.suffix, key.data() + l.shared);
		at = {l.index + 1, l.p, l.s};
		return size;
	}

	// Builds the block's last key into out without decoding the whole last run: scans the
	// run's headers, then decodes from the last entry back only the suffix symbols the key
	// keeps (each entry supplies the key bytes from its shared length up to the next
	// entry's).
	void last_key(std::string& out) const {
		out.clear();
		if (count_ == 0) { return; }
		const std::size_t r = restart_count_ - 1, n = count_ - r * restart_interval;
		const char* p = nullptr;
		suffixes s = run(r, p);
		const suffixes base = s;
		std::uint64_t shared[restart_interval], suffix[restart_interval], bit[restart_interval], length = 0;
		for (std::size_t i = 0; i < n; ++i) {
			std::uint64_t value = 0;
			header(p, end_, shared[i], suffix[i], value);
			p += value;
			if (shared[i] > length) { detail::fail(); }  // the first entry has no predecessor
			s.start(suffix[i]);
			bit[i] = s.at_;
			s.skip(suffix[i]);
			length = shared[i] + suffix[i];
		}
		out.resize(length);
		for (std::size_t i = n, need = length; i-- && need;) {
			if (need <= shared[i]) { continue; }
			suffixes e = base;
			e.at_ = bit[i];
			e.decode(std::min<std::uint64_t>(suffix[i], need - shared[i]), out.data() + shared[i]);
			need = shared[i];
		}
	}

public:
	// Finds key's value by walking restart run r (walk_to's rule); visit(value) sees each
	// entry's value from the restart through key's, in order.
	template <typename Visit>
	std::optional<std::string_view> find_from(std::size_t r, std::string_view key, Visit&& visit) const {
		landing at;
		if (walk_to(r, key, visit, at) && at.equal) { return at.value; }
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
		cursor c(*this);
		for (bool more = c.seek(from); more; more = c.next()) {
			if (!f(c.key(), c.value())) { return; }
		}
	}

	// Every entry in key order, pulled one at a time (a k-way merge of dictionaries).
	// Holds one decoded block; neither copied nor moved, since it points into the reader.
	class cursor {
	public:
		explicit cursor(const reader& r) : r_(&r) {}
		cursor(const cursor&) = delete;
		cursor& operator=(const cursor&) = delete;

		// Moves to the next entry; false past the last.
		bool next() {
			on_entry_ = false;
			if (at_.index == count_) [[unlikely]] {
				if (next_ >= r_->blocks_.size()) { return false; }
				open(next_);
			}
			size_ = block_->step<true>(at_, key_, size_, value_);
			on_entry_ = true;
			return true;
		}
		// Moves to the first entry with key >= from, from any position; false when there is
		// none, and next() then returns false too. Starts at from's block and restart run:
		// one index lookup, a binary search of the run starts, and one run walked without
		// building keys.
		bool seek(std::string_view from) {
			const bool ahead = on_entry_;
			on_entry_ = false;
			if (ahead && forward(from)) {
				on_entry_ = true;
				return true;
			}
			const auto offset = r_->index_.floor_value(from);
			if (!offset) {  // from sorts before every key
				block_.reset();
				at_ = {};
				count_ = 0;
				next_ = 0;
				return next();
			}
			open(*offset);
			const block& b = *block_;
			std::size_t lo = 0, hi = b.restarts();  // the last run starting at or below from
			while (hi - lo > 1) {
				const std::size_t mid = (lo + hi) / 2;
				if (b.compare_restart(mid, from) <= 0) { lo = mid; } else { hi = mid; }
			}
			block::landing l;
			if (!b.walk_to(lo, from, [](std::string_view) {}, l)) {
				at_.index = std::min(count_, (lo + 1) * restart_interval);  // the next run's first entry
				return next();
			}
			size_ = b.land(l, from, key_, at_);
			value_ = l.value;
			on_entry_ = true;
			return true;
		}

		std::string_view key() const noexcept { return {key_.data(), size_}; }
		std::string_view value() const noexcept { return value_; }

	private:
		// A seek to from at or above the current key and at or below the open block's last
		// key: searches the block's later restart runs, or finishes the current run, without
		// the index lookup. False (cursor untouched) when from sorts past the block's last
		// key; one comparison against the cached last key decides.
		bool forward(std::string_view from) {
			const block& b = *block_;
			const int c = from.compare(std::string_view(key_.data(), size_));
			if (c < 0) { return false; }
			if (c == 0) { return true; }
			if (!last_known_) {
				b.last_key(last_);
				last_known_ = true;
			}
			if (from.compare(last_) > 0) { return false; }
			const std::size_t run = (at_.index - 1) / restart_interval, runs = b.restarts();
			std::size_t lo = run, hi = runs;  // the last run starting at or below from
			while (hi - lo > 1) {
				const std::size_t mid = (lo + hi) / 2;
				if (b.compare_restart(mid, from) <= 0) { lo = mid; } else { hi = mid; }
			}
			if (lo == run) {
				const std::size_t end = std::min(count_, (run + 1) * restart_interval);
				while (at_.index < end) {
					size_ = b.step<true>(at_, key_, size_, value_);
					if (std::string_view(key_.data(), size_).compare(from) >= 0) { return true; }
				}
				return at_.index < count_ && next();  // the next run's first key is above from
			}
			block::landing l;
			if (!b.walk_to(lo, from, [](std::string_view) {}, l)) { return false; }  // only unsorted data
			size_ = b.land(l, from, key_, at_);
			value_ = l.value;
			return true;
		}

		void open(std::uint64_t offset) {
			block_.emplace(r_->open(offset));
			at_ = {};
			count_ = block_->count();
			next_ = block_->next_block();
			last_known_ = false;
		}

		const reader* r_;
		std::optional<block> block_;
		block::position at_;
		std::size_t count_ = 0;  // the open block's entries
		std::size_t size_ = 0;   // the key's length; key_ holds it, padded, and never shrinks
		std::uint64_t next_ = 0;  // the next block's offset
		bool on_entry_ = false;   // key_ is the cursor's current entry (seek and next returned true)
		bool last_known_ = false;  // last_ is the open block's last key
		std::string last_;
		std::vector<char> key_;
		std::string_view value_;
	};

private:
	// The block at offset, verified first when a verifier is set: its length prefix, then
	// the whole block.
	block open(std::uint64_t offset) const {
		if (verify_ && offset < blocks_.size()) {
			const std::uint64_t head = std::min<std::uint64_t>(varint::leb128::max_bytes, blocks_.size() - offset);
			if (!verify_(context_, offset, head)) { detail::fail(); }
			const char* p = blocks_.data() + offset;
			std::uint64_t length = 0;
			if (!varint::leb128::decode(&p, blocks_.data() + offset + head, length) || length > std::uint64_t(blocks_.data() + blocks_.size() - p)) { detail::fail(); }
			if (!verify_(context_, offset, std::uint64_t(p - (blocks_.data() + offset)) + length)) { detail::fail(); }
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
