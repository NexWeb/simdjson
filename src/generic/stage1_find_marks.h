// This file contains the common code every implementation uses in stage1
// It is intended to be included multiple times and compiled multiple times
// We assume the file in which it is included already includes
// "simdjson/stage1_find_marks.h" (this simplifies amalgation)

// return a bitvector indicating where we have characters that end an odd-length
// sequence of backslashes (and thus change the behavior of the next character
// to follow). A even-length sequence of backslashes, and, for that matter, the
// largest even-length prefix of our odd-length sequence of backslashes, simply
// modify the behavior of the backslashes themselves.
// We also update the prev_iter_ends_odd_backslash reference parameter to
// indicate whether we end an iteration on an odd-length sequence of
// backslashes, which modifies our subsequent search for odd-length
// sequences of backslashes in an obvious way.
really_inline uint64_t find_odd_backslash_sequences(
    uint64_t bs_bits,
    bool &prev_iter_ends_odd_backslash) {
  const uint64_t even_bits = 0x5555555555555555ULL;
  const uint64_t odd_bits = ~even_bits;
  uint64_t start_edges = bs_bits & ~(bs_bits << 1);
  /* flip lowest if we have an odd-length run at the end of the prior
  * iteration */
  uint64_t even_start_mask = even_bits ^ prev_iter_ends_odd_backslash;
  uint64_t even_starts = start_edges & even_start_mask;
  uint64_t odd_starts = start_edges & ~even_start_mask;
  uint64_t even_carries = bs_bits + even_starts;

  uint64_t odd_carries;
  /* must record the carry-out of our odd-carries out of bit 63; this
  * indicates whether the sense of any edge going to the next iteration
  * should be flipped */
  bool iter_ends_odd_backslash =
      add_overflow(bs_bits, odd_starts, &odd_carries);

  odd_carries |= prev_iter_ends_odd_backslash; /* push in bit zero as a
                                                * potential end if we had an
                                                * odd-numbered run at the
                                                * end of the previous
                                                * iteration */
  prev_iter_ends_odd_backslash = iter_ends_odd_backslash != 0;
  uint64_t even_carry_ends = even_carries & ~bs_bits;
  uint64_t odd_carry_ends = odd_carries & ~bs_bits;
  uint64_t even_start_odd_end = even_carry_ends & odd_bits;
  uint64_t odd_start_even_end = odd_carry_ends & even_bits;
  uint64_t odd_ends = even_start_odd_end | odd_start_even_end;
  return odd_ends;
}

// return both the quote mask (which is a half-open mask that covers the first
// quote
// in an unescaped quote pair and everything in the quote pair) and the quote
// bits, which are the simple
// unescaped quoted bits. We also update the prev_iter_inside_quote value to
// tell the next iteration
// whether we finished the final iteration inside a quote pair; if so, this
// inverts our behavior of
// whether we're inside quotes for the next iteration.
// Note that we don't do any error checking to see if we have backslash
// sequences outside quotes; these
// backslash sequences (of any length) will be detected elsewhere.
really_inline uint64_t find_quote_mask_and_bits(
    uint64_t unescaped, uint64_t odd_ends,
    uint64_t &prev_iter_inside_quote, uint64_t &quote_bits,
    uint64_t &error_mask) {
  quote_bits = quote_bits & ~odd_ends;
  uint64_t quote_mask = compute_quote_mask(quote_bits);
  quote_mask ^= prev_iter_inside_quote;
  error_mask |= quote_mask & unescaped;
  /* right shift of a signed value expected to be well-defined and standard
   * compliant as of C++20,
   * John Regher from Utah U. says this is fine code */
  prev_iter_inside_quote =
      static_cast<uint64_t>(static_cast<int64_t>(quote_mask) >> 63);
  return quote_mask;
}

really_inline uint64_t finalize_structurals(
    uint64_t structurals, uint64_t whitespace, uint64_t quote_mask,
    uint64_t quote_bits, bool &prev_iter_ends_pseudo_pred) {
  // mask off anything inside quotes
  structurals &= ~quote_mask;
  // add the real quote bits back into our bit_mask as well, so we can
  // quickly traverse the strings we've spent all this trouble gathering
  structurals |= quote_bits;
  // Now, establish "pseudo-structural characters". These are non-whitespace
  // characters that are (a) outside quotes and (b) have a predecessor that's
  // either whitespace or a structural character. This means that subsequent
  // passes will get a chance to encounter the first character of every string
  // of non-whitespace and, if we're parsing an atom like true/false/null or a
  // number we can stop at the first whitespace or structural character
  // following it.

  // a qualified predecessor is something that can happen 1 position before an
  // pseudo-structural character
  uint64_t pseudo_pred = structurals | whitespace;

  uint64_t shifted_pseudo_pred =
      (pseudo_pred << 1) | prev_iter_ends_pseudo_pred;
  prev_iter_ends_pseudo_pred = (pseudo_pred >> 63) != 0;
  uint64_t pseudo_structurals =
      shifted_pseudo_pred & (~whitespace) & (~quote_mask);
  structurals |= pseudo_structurals;

  // now, we've used our close quotes all we need to. So let's switch them off
  // they will be off in the quote mask and on in quote bits.
  structurals &= ~(quote_bits & ~quote_mask);
  return structurals;
}

// Find structural bits in an input chunk.
really_inline void find_structural_bits(
    const uint8_t *in_buf, size_t &idx, uint32_t *base_ptr, uint32_t &base,
    bool &prev_iter_ends_odd_backslash, uint64_t &prev_iter_inside_quote,
    bool &prev_iter_ends_pseudo_pred, uint64_t &prev_structurals,
    uint64_t &error_mask,
    utf8_checker &utf8_state) {

    each64([&](size_t chunk) {
      simd_input64 in64(in_buf+(chunk*64));
      utf8_state.check_next_input(in64);
    });

    bitmask_array odd_ends = map64([&](size_t chunk) {
      simd_input64 in64(in_buf+(chunk*64));
      /* detect odd sequences of backslashes */
      return find_odd_backslash_sequences(in64.eq('\\'), prev_iter_ends_odd_backslash);
    });

    bitmask_array quote_bits;
    bitmask_array quote_mask = map64([&](size_t chunk) {
      simd_input64 in64(in_buf+(chunk*64));

      /* detect insides of quote pairs ("quote_mask") and also our quote_bits
      * themselves */
      quote_bits[chunk] = in64.eq('"');
      /* All Unicode characters may be placed within the
      * quotation marks, except for the characters that MUST be escaped:
      * quotation mark, reverse solidus, and the control characters (U+0000
      * through U+001F).
      * https://tools.ietf.org/html/rfc8259 */
      uint64_t unescaped = in64.lteq(0x1F);
      return find_quote_mask_and_bits(
          unescaped, odd_ends[chunk], prev_iter_inside_quote, quote_bits[chunk], error_mask);
    });

    bitmask_array whitespace;
    bitmask_array structurals;
    each64([&](uint64_t chunk) {
      simd_input64 in64(in_buf+(chunk*64));
      find_whitespace_and_structurals(in64, whitespace[chunk], structurals[chunk]);
    });

    each64([&](uint64_t chunk) {
      /* take the previous iterations structural bits, not our current
      * iteration,
      * and flatten */
      flatten_bits(base_ptr, base, idx, prev_structurals);

      /* fixup structurals to reflect quotes and add pseudo-structural
      * characters */
      prev_structurals = finalize_structurals(structurals[chunk], whitespace[chunk], quote_mask[chunk],
                                              quote_bits[chunk], prev_iter_ends_pseudo_pred);

      idx += 64;
    });
}

int find_structural_bits(const uint8_t *buf, size_t len, simdjson::ParsedJson &pj) {
  if (len > pj.byte_capacity) {
    std::cerr << "Your ParsedJson object only supports documents up to "
              << pj.byte_capacity << " bytes but you are trying to process "
              << len << " bytes" << std::endl;
    return simdjson::CAPACITY;
  }
  uint32_t *base_ptr = pj.structural_indexes;
  uint32_t base = 0;
  utf8_checker utf8_state;

  /* we have padded the input out to CHUNK_SIZE (e.g. 64) multiple with the
   * remainder being zeros persistent state across loop does the last iteration end
   * with an odd-length sequence of backslashes? */

  bool prev_iter_ends_odd_backslash = false;
  /* does the previous iteration end inside a double-quote pair? */
  uint64_t prev_iter_inside_quote =
      0ULL; /* either all zeros or all ones
             * does the previous iteration end on something that is a
             * predecessor of a pseudo-structural character - i.e.
             * whitespace or a structural character effectively the very
             * first char is considered to follow "whitespace" for the
             * purposes of pseudo-structural character detection so we
             * initialize to 1 */
  bool prev_iter_ends_pseudo_pred = true;

  /* structurals are persistent state across loop as we flatten them on the
   * subsequent iteration into our array pointed to be base_ptr.
   * This is harmless on the first iteration as structurals==0
   * and is done for performance reasons; we can hide some of the latency of
   * the
   * expensive carryless multiply in the previous step with this work */
  uint64_t prev_structurals = 0;

  size_t last_chunk_idx = len < SIMD_WIDTH ? 0 : len - SIMD_WIDTH;
  size_t idx = 0;
  uint64_t error_mask = 0; /* for unescaped characters within strings (ASCII
                          code points < 0x20) */

  while (idx < last_chunk_idx) {
    find_structural_bits(&buf[idx], idx, base_ptr, base,
                         prev_iter_ends_odd_backslash,
                         prev_iter_inside_quote, prev_iter_ends_pseudo_pred,
                         prev_structurals, error_mask, utf8_state);
  }
  /* If we have a final chunk of less than CHUNK_SIZE bytes, pad it with spaces
   * before processing it (otherwise, we risk invalidating the UTF-8 checks). */
  if (idx < len) {
    uint8_t tmp_buf[SIMD_WIDTH];
    memset(tmp_buf, 0x20, SIMD_WIDTH);
    memcpy(tmp_buf, buf + idx, len - idx);
    find_structural_bits(&tmp_buf[0], idx, base_ptr, base,
                         prev_iter_ends_odd_backslash,
                         prev_iter_inside_quote, prev_iter_ends_pseudo_pred,
                         prev_structurals, error_mask, utf8_state);
  }

  /* is last string quote closed? */
  if (prev_iter_inside_quote) {
    return simdjson::UNCLOSED_STRING;
  }

  /* finally, flatten out the remaining structurals from the last iteration
   */
  flatten_bits(base_ptr, base, idx, prev_structurals);

  pj.n_structural_indexes = base;
  /* a valid JSON file cannot have zero structural indexes - we should have
   * found something */
  if (pj.n_structural_indexes == 0u) {
    return simdjson::EMPTY;
  }
  if (base_ptr[pj.n_structural_indexes - 1] > len) {
    return simdjson::UNEXPECTED_ERROR;
  }
  if (len != base_ptr[pj.n_structural_indexes - 1]) {
    /* the string might not be NULL terminated, but we add a virtual NULL
     * ending character. */
    base_ptr[pj.n_structural_indexes++] = len;
  }
  /* make it safe to dereference one beyond this array */
  base_ptr[pj.n_structural_indexes] = 0;
  if (error_mask) {
    return simdjson::UNESCAPED_CHARS;
  }
  return utf8_state.errors();
}