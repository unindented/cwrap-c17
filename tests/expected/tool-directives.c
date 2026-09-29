void close_borrowed(FILE* stream) {
  // The caller owns the stream.
  // cppcheck-suppress deallocuse
  TEST_CHECK(fclose(stream) == 0);
}

// Prose before a directive joins, and
// so does the prose
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
// after it, which starts a new
// paragraph.
int pair(int a, int b);

/*
 * A block comment keeps its directive
 * lines too.
 * NOLINTBEGIN(misc-no-recursion)
 */

// clang-format off
// IWYU pragma: keep
// NOLINTED is only prose, so it joins.

// Prose that mentions the directive by
// name, such as clang-format off
// or NOLINT, never starts a line with
// it.
