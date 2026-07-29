// RealWorld C++ corpus (FR-46), project `tokenizer`: lexer body.
// See lexer.hpp for the shape rationale.
#include "lexer.hpp"

static int classify(char c) {
  if (c >= '0' && c <= '9')
    return TOK_NUMBER;
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
    return TOK_WORD;
  if (c == ' ' || c == '\t' || c == '\n')
    return TOK_NONE;
  return TOK_PUNCT;
}

Lexer::Lexer()
    : state_(TOK_NONE), run_(0), words_(0), numbers_(0), punct_(0),
      longest_(0) {}

void Lexer::feed(char c) {
  int kind = classify(c);
  if (kind == state_ && kind != TOK_PUNCT && kind != TOK_NONE) {
    run_ = run_ + 1;
    return;
  }
  finish();
  state_ = kind;
  run_ = 1;
  if (kind == TOK_PUNCT) {
    punct_ = punct_ + 1;
    state_ = TOK_NONE;
    run_ = 0;
  }
}

void Lexer::finish() {
  if (state_ == TOK_WORD) {
    words_ = words_ + 1;
    if (run_ > longest_)
      longest_ = run_;
  } else if (state_ == TOK_NUMBER) {
    numbers_ = numbers_ + 1;
  }
  state_ = TOK_NONE;
  run_ = 0;
}

int Lexer::words() const { return words_; }
int Lexer::numbers() const { return numbers_; }
int Lexer::punct() const { return punct_; }
int Lexer::longest_word() const { return longest_; }
