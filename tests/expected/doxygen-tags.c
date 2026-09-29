/**
 * \brief Parses a value.
 * \param text Input text.
 * \return Zero on success.
 */
int parse(const char* text);

/**
 * @brief Opens a file.
 * @retval 0  Success.
 * @retval -1 Failure.
 * @note Not thread-safe.
 * @see close_file
 */
int open_file(void);

/**
 * Pass the handle from open_file,
 * then @p n bytes.
 */
void use_file(int handle, size_t n);
