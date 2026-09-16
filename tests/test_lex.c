#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "core/arena.h"
#include "domain/lex.h"

/**
 * @brief Lexes source text into comment spans.
 *
 * @param arena  Arena that owns the returned list storage.
 * @param source Terminated source text to lex.
 * @return The comment-span list.
 */
static struct CommentSpanList lex(struct Arena* arena, const char* source) {
  struct CommentSpanList spans;
  char err[64];
  TEST_CHECK(lex_comment_spans(source, strlen(source), arena, &spans, err, sizeof(err)) == 0);
  return spans;
}

// A line comment and a block comment are recorded as separate spans.
static void test_records_line_and_block_comments(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentSpanList spans = lex(&arena, "// line\n/* block */\n");
  TEST_CHECK(spans.count == 2);
  TEST_CHECK(spans.items[0].kind == COMMENT_KIND_LINE);
  TEST_CHECK(spans.items[0].start == 0);
  TEST_CHECK(spans.items[1].kind == COMMENT_KIND_BLOCK);
  arena_free(&arena);
}

// A URL inside a string is not a comment, including doubled slashes.
static void test_string_url_yields_no_comments(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentSpanList spans = lex(&arena, "char* u = \"http://x//y\";\n");
  TEST_CHECK(spans.count == 0);
  arena_free(&arena);
}

// Adjacent one-line block comments stay separate spans.
static void test_adjacent_block_comments_stay_separate(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentSpanList spans = lex(&arena, "/* show */ /* two */\n");
  TEST_CHECK(spans.count == 2);
  TEST_CHECK(spans.items[0].end < spans.items[1].start);
  arena_free(&arena);
}

// A comment after code on the same line is marked trailing.
static void test_trailing_comment_is_flagged(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentSpanList spans = lex(&arena, "int x; /* trailing */\n");
  TEST_CHECK(spans.count == 1);
  TEST_CHECK(spans.items[0].is_trailing);
  arena_free(&arena);
}

// A comment at the start of a line is not trailing.
static void test_leading_comment_is_not_trailing(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentSpanList spans = lex(&arena, "  /* leading */\nint x;\n");
  TEST_CHECK(spans.count == 1);
  TEST_CHECK(!spans.items[0].is_trailing);
  TEST_CHECK(spans.items[0].opener_column == 2);
  arena_free(&arena);
}

// An include path is not treated as a string that could hide a later comment.
static void test_include_path_does_not_hide_comment(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentSpanList spans = lex(&arena, "#include \"http://x//y\" /* show */\n");
  TEST_CHECK(spans.count == 1);
  TEST_CHECK(spans.items[0].is_trailing);
  TEST_CHECK(spans.items[0].kind == COMMENT_KIND_BLOCK);
  arena_free(&arena);
}

// Slashes inside either header-name form are not comments.
static void test_include_paths_yield_no_false_comments(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentSpanList quoted = lex(&arena, "#include \"http://x//y\"\n");
  TEST_CHECK(quoted.count == 0);
  struct CommentSpanList angled = lex(&arena, "#include <http://x//y>\n");
  TEST_CHECK(angled.count == 0);
  struct CommentSpanList prefixed = lex(&arena, "#include_extra \"http://x//y\"\n");
  TEST_CHECK(prefixed.count == 0);
  struct CommentSpanList spliced = lex(&arena, "#include \"http://x\\\n//y\" /* trailing */\n");
  TEST_CHECK(spliced.count == 1);
  TEST_CHECK(spliced.items[0].kind == COMMENT_KIND_BLOCK);
  arena_free(&arena);
}

// A backslash-newline preserves code on the logical line, so the following comment is trailing.
static void test_code_splice_preserves_trailing_status(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentSpanList spans = lex(&arena, "#define X \\\n  /* trailing */\n");
  TEST_CHECK(spans.count == 1);
  TEST_CHECK(spans.items[0].is_trailing);
  arena_free(&arena);
}

// A character literal containing a quote does not start a string.
static void test_char_literal_quote_is_not_a_string(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentSpanList spans = lex(&arena, "char q = '\\''; /* keep */\n");
  TEST_CHECK(spans.count == 1);
  arena_free(&arena);
}

// CRLF line splices keep the lexer inside string and character literals.
static void test_crlf_splices_preserve_literal_state(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentSpanList string_spans =
      lex(&arena, "const char* u = \"http:\\\r\n//example.com\";\r\n");
  TEST_CHECK(string_spans.count == 0);
  struct CommentSpanList char_spans = lex(&arena, "int c = '\\\r\n//';\r\n");
  TEST_CHECK(char_spans.count == 0);
  arena_free(&arena);
}

TEST_LIST = {
    {"records line and block comments", test_records_line_and_block_comments},
    {"string url yields no comments", test_string_url_yields_no_comments},
    {"adjacent block comments stay separate", test_adjacent_block_comments_stay_separate},
    {"trailing comment is flagged", test_trailing_comment_is_flagged},
    {"leading comment is not trailing", test_leading_comment_is_not_trailing},
    {"include path does not hide comment", test_include_path_does_not_hide_comment},
    {"include paths yield no false comments", test_include_paths_yield_no_false_comments},
    {"code splice preserves trailing status", test_code_splice_preserves_trailing_status},
    {"char literal quote is not a string", test_char_literal_quote_is_not_a_string},
    {"crlf splices preserve literal state", test_crlf_splices_preserve_literal_state},
    {NULL, NULL},
};
