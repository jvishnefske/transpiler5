// RealWorld C++ corpus (FR-46), project `tokenizer`: a streaming lexer.
//
// The MEMBER-FUNCTION member of the corpus: a class with a constructor using a
// member-initializer list, one mutating method, and several `const` accessors
// -- and deliberately NO virtual method, NO base class, NO reference, NO
// template, NO pointer data member. Chosen because a character-at-a-time
// state-machine lexer is the canonical shape of hand-written parsers in
// embedded and protocol code: all state is scalar, all input is pushed in, and
// the object never owns a heap buffer. It is the direct realistic analogue of
// what the W2.2 method wave landed on feature fixtures.
#ifndef TOKENIZER_LEXER_HPP
#define TOKENIZER_LEXER_HPP

/// Token classes the lexer counts.
enum TokenKind {
  TOK_NONE = 0,
  TOK_WORD = 1,
  TOK_NUMBER = 2,
  TOK_PUNCT = 3
};

/// A character-at-a-time lexer that classifies runs of input into words,
/// numbers, and punctuation, tracking the longest word seen.
class Lexer {
public:
  Lexer();

  /// Folds one input character into the running classification.
  void feed(char c);

  /// Closes any run still open (call once after the last `feed`).
  void finish();

  int words() const;
  int numbers() const;
  int punct() const;
  int longest_word() const;

private:
  int state_;   ///< A TokenKind: the run currently open, or TOK_NONE.
  int run_;     ///< Length of the currently open run.
  int words_;
  int numbers_;
  int punct_;
  int longest_;
};

#endif
