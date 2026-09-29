/**
 * @brief Copies `n` bytes from `src` to `dest`. The two
 *        regions must not overlap.
 * @param dest Destination buffer. It must be large enough for
 *             `n` bytes.
 * @param n
 *        Number of bytes.
 * @note Short
 *                  line.
 * @return
 *     `dest`.
 */
void* copy(void* dest, const void* src, size_t n);

/**
 * @param len Byte count of buffers is 0. Untouched.
 */
void clear(size_t len);
