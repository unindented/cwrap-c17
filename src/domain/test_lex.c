#include <acutest.h>
#include <stddef.h>

#include "domain/lex.h"
#include "shared/arena.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Lexes source text into comment spans.
 *
 * @param source Terminated source text to lex.
 * @param arena  Arena that owns the returned list storage. Must not be `NULL`.
 * @return The comment-span list.
 */
static struct CommentSpanList lex(const char* source, struct Arena* arena) {
  return lex_comment_spans(str_from_cstr(source), arena);
}

// A line comment and a block comment are recorded as separate spans.
static void test_records_line_and_block_comments(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList spans = lex("// line\n/* block */\n", &arena);
  TEST_CHECK(spans.count == 2);
  TEST_CHECK(spans.items[0].kind == COMMENT_KIND_LINE);
  TEST_CHECK(spans.items[0].start == 0);
  TEST_CHECK(spans.items[1].kind == COMMENT_KIND_BLOCK);
}

// A URL inside a string is not a comment, including doubled slashes.
static void test_string_url_yields_no_comments(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList spans = lex("char* u = \"http://x//y\";\n", &arena);
  TEST_CHECK(spans.count == 0);
}

// Adjacent one-line block comments stay separate spans.
static void test_adjacent_block_comments_stay_separate(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList spans = lex("/* show */ /* two */\n", &arena);
  TEST_CHECK(spans.count == 2);
  TEST_CHECK(spans.items[0].end < spans.items[1].start);
}

// A comment after code on the same line is marked trailing.
static void test_trailing_comment_is_flagged(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList spans = lex("int x; /* trailing */\n", &arena);
  TEST_CHECK(spans.count == 1);
  TEST_CHECK(spans.items[0].is_trailing);
}

// A comment after another comment's closer on the same line is marked trailing.
static void test_comment_after_comment_is_trailing(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList spans = lex("/* a */ /* b */\n/* c\n */ // d\n", &arena);
  TEST_CHECK(spans.count == 4);
  TEST_CHECK(!spans.items[0].is_trailing);
  TEST_CHECK(spans.items[1].is_trailing);
  TEST_CHECK(!spans.items[2].is_trailing);
  TEST_CHECK(spans.items[3].is_trailing);
}

// A comment at the start of a line is not trailing.
static void test_leading_comment_is_not_trailing(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList spans = lex("  /* leading */\nint x;\n", &arena);
  TEST_CHECK(spans.count == 1);
  TEST_CHECK(!spans.items[0].is_trailing);
  TEST_CHECK(spans.items[0].opener_column == 2);
}

// A quoted header name closes at its quote, so a comment after it is found and is trailing.
static void test_include_path_does_not_hide_comment(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList spans = lex("#include \"http://x//y\" /* show */\n", &arena);
  TEST_CHECK(spans.count == 1);
  TEST_CHECK(spans.items[0].is_trailing);
  TEST_CHECK(spans.items[0].kind == COMMENT_KIND_BLOCK);
}

// Slashes inside a quoted header name are not comments.
static void test_quoted_include_path_yields_no_false_comments(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList quoted = lex("#include \"http://x//y\"\n", &arena);
  TEST_CHECK(quoted.count == 0);
  struct CommentSpanList spliced = lex("#include \"http://x\\\n//y\" /* trailing */\n", &arena);
  TEST_CHECK(spliced.count == 1);
  TEST_CHECK(spliced.items[0].kind == COMMENT_KIND_BLOCK);
}

// A backslash-newline preserves code on the logical line, so the following comment is trailing.
static void test_code_splice_preserves_trailing_status(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList spans = lex("#define X \\\n  /* trailing */\n", &arena);
  TEST_CHECK(spans.count == 1);
  TEST_CHECK(spans.items[0].is_trailing);
}

// LF and CRLF line splices keep the lexer inside a line comment, so a slash-star opener on the
// spliced line starts no block comment.
static void test_splices_extend_line_comment(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList lf = lex("// a \\\n/* b\nint x;\n", &arena);
  TEST_CHECK(lf.count == 1);
  TEST_CHECK(lf.items[0].kind == COMMENT_KIND_LINE);
  TEST_CHECK(lf.items[0].end == S("// a \\\n/* b").len);
  struct CommentSpanList crlf = lex("// a \\\r\n/* b\r\nint x;\r\n", &arena);
  TEST_CHECK(crlf.count == 1);
  TEST_CHECK(crlf.items[0].end == S("// a \\\r\n/* b").len);
}

// A character literal containing a quote does not start a string.
static void test_char_literal_quote_is_not_a_string(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList spans = lex("char q = '\\''; /* keep */\n", &arena);
  TEST_CHECK(spans.count == 1);
}

// CRLF line splices keep the lexer inside string and character literals.
static void test_crlf_splices_preserve_literal_state(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList string_spans =
      lex("const char* u = \"http:\\\r\n//example.com\";\r\n", &arena);
  TEST_CHECK(string_spans.count == 0);
  struct CommentSpanList char_spans = lex("int c = '\\\r\n//';\r\n", &arena);
  TEST_CHECK(char_spans.count == 0);
}

// The source ends where its view does: a line comment cut short by the end of the view ends there,
// and a lone slash at the end opens nothing.
static void test_source_ends_at_view_end(void) {
  struct Arena arena = test_arena();
  struct CommentSpanList spans = lex_comment_spans(str_head(S("// a\n/* b */"), 6), &arena);
  TEST_CHECK(spans.count == 1);
  TEST_CHECK(spans.items[0].kind == COMMENT_KIND_LINE);
  TEST_CHECK(spans.items[0].end == S("// a").len);
  spans = lex_comment_spans(str_head(S("x // a"), 4), &arena);
  TEST_CHECK(spans.count == 1);
  TEST_CHECK(spans.items[0].end == 4);
}

TEST_LIST = {
    {"records line and block comments", test_records_line_and_block_comments},
    {"string url yields no comments", test_string_url_yields_no_comments},
    {"adjacent block comments stay separate", test_adjacent_block_comments_stay_separate},
    {"trailing comment is flagged", test_trailing_comment_is_flagged},
    {"comment after comment is trailing", test_comment_after_comment_is_trailing},
    {"leading comment is not trailing", test_leading_comment_is_not_trailing},
    {"include path does not hide comment", test_include_path_does_not_hide_comment},
    {"quoted include path yields no false comments",
     test_quoted_include_path_yields_no_false_comments},
    {"code splice preserves trailing status", test_code_splice_preserves_trailing_status},
    {"splices extend line comment", test_splices_extend_line_comment},
    {"char literal quote is not a string", test_char_literal_quote_is_not_a_string},
    {"crlf splices preserve literal state", test_crlf_splices_preserve_literal_state},
    {"source ends at view end", test_source_ends_at_view_end},
    {NULL, NULL},
};
