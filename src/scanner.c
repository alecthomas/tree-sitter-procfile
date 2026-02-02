#include "tree_sitter/parser.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum TokenType {
  NEWLINE,
  INDENT,
  DEDENT,
  COMMAND_TEXT,
  MULTILINE_COMMAND_TEXT,
  LINE_CONTINUATION,
  OPTION_KEY,
  BARE_GLOB,
};

typedef struct {
  bool in_multiline_block;
  uint32_t block_indent;
} Scanner;

static void advance(TSLexer *lexer) {
  lexer->advance(lexer, false);
}

static void skip(TSLexer *lexer) {
  lexer->advance(lexer, true);
}

static bool is_space(int32_t c) {
  return c == ' ' || c == '\t';
}

void *tree_sitter_procfile_external_scanner_create(void) {
  Scanner *scanner = calloc(1, sizeof(Scanner));
  return scanner;
}

void tree_sitter_procfile_external_scanner_destroy(void *payload) {
  free(payload);
}

unsigned tree_sitter_procfile_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *scanner = (Scanner *)payload;
  buffer[0] = scanner->in_multiline_block;
  memcpy(&buffer[1], &scanner->block_indent, sizeof(uint32_t));
  return 1 + sizeof(uint32_t);
}

void tree_sitter_procfile_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *scanner = (Scanner *)payload;
  if (length >= 1 + sizeof(uint32_t)) {
    scanner->in_multiline_block = buffer[0];
    memcpy(&scanner->block_indent, &buffer[1], sizeof(uint32_t));
  } else {
    scanner->in_multiline_block = false;
    scanner->block_indent = 0;
  }
}

static bool scan_line_continuation(TSLexer *lexer) {
  if (lexer->lookahead != '\\') return false;
  advance(lexer);

  // Skip spaces after backslash
  while (is_space(lexer->lookahead)) {
    advance(lexer);
  }

  if (lexer->lookahead == '\n') {
    advance(lexer);
    // Skip leading whitespace on continued line
    while (is_space(lexer->lookahead)) {
      advance(lexer);
    }
    lexer->result_symbol = LINE_CONTINUATION;
    return true;
  }
  return false;
}

static bool scan_newline(TSLexer *lexer, Scanner *scanner) {
  if (lexer->lookahead != '\n') return false;

  lexer->result_symbol = NEWLINE;
  advance(lexer);
  lexer->mark_end(lexer);
  return true;
}

static bool scan_indent(TSLexer *lexer, Scanner *scanner) {
  // We're at the start of a line, count indentation
  uint32_t indent = 0;
  while (is_space(lexer->lookahead)) {
    indent++;
    advance(lexer);
  }

  // Skip blank lines
  if (lexer->lookahead == '\n' || lexer->eof(lexer)) {
    return false;
  }

  if (!scanner->in_multiline_block && indent > 0) {
    // Entering a multiline block
    scanner->in_multiline_block = true;
    scanner->block_indent = indent;
    lexer->result_symbol = INDENT;
    return true;
  }

  return false;
}

static bool scan_dedent(TSLexer *lexer, Scanner *scanner) {
  if (!scanner->in_multiline_block) return false;

  // Check indentation at start of line
  uint32_t indent = 0;
  while (is_space(lexer->lookahead)) {
    indent++;
    advance(lexer);
  }

  // If we're at a non-indented line (or less indented), emit dedent
  if (indent < scanner->block_indent && lexer->lookahead != '\n' && !lexer->eof(lexer)) {
    scanner->in_multiline_block = false;
    scanner->block_indent = 0;
    lexer->result_symbol = DEDENT;
    return true;
  }

  // Also dedent at EOF
  if (lexer->eof(lexer) && scanner->in_multiline_block) {
    scanner->in_multiline_block = false;
    scanner->block_indent = 0;
    lexer->result_symbol = DEDENT;
    return true;
  }

  return false;
}

static bool scan_command_text(TSLexer *lexer) {
  bool has_content = false;

  while (lexer->lookahead != '\n' && !lexer->eof(lexer)) {
    // Check for line continuation
    if (lexer->lookahead == '\\') {
      lexer->mark_end(lexer);
      advance(lexer);
      while (is_space(lexer->lookahead)) {
        advance(lexer);
      }
      if (lexer->lookahead == '\n') {
        // This is a line continuation, stop here
        lexer->result_symbol = COMMAND_TEXT;
        return has_content;
      }
      // Not a line continuation, the backslash is part of the command
      has_content = true;
      continue;
    }
    advance(lexer);
    has_content = true;
  }

  if (has_content) {
    lexer->mark_end(lexer);
    lexer->result_symbol = COMMAND_TEXT;
    return true;
  }
  return false;
}

static bool scan_multiline_command_text(TSLexer *lexer) {
  bool has_content = false;

  while (lexer->lookahead != '\n' && !lexer->eof(lexer)) {
    advance(lexer);
    has_content = true;
  }

  if (has_content) {
    lexer->mark_end(lexer);
    lexer->result_symbol = MULTILINE_COMMAND_TEXT;
    return true;
  }
  return false;
}

static bool is_option_key_char(int32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static bool is_option_key_start(int32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// Scan an option key only if followed by '='
// This uses lookahead to distinguish "ready=5432" (option) from "Procfile" (glob)
static bool scan_option_key(TSLexer *lexer) {
  if (!is_option_key_start(lexer->lookahead)) return false;
  
  // Mark start position so we can check if followed by '=' before committing
  lexer->mark_end(lexer);
  
  advance(lexer);
  while (is_option_key_char(lexer->lookahead)) {
    advance(lexer);
  }
  
  // Only match if followed by '='
  if (lexer->lookahead == '=') {
    lexer->mark_end(lexer);
    lexer->result_symbol = OPTION_KEY;
    return true;
  }
  
  // Not followed by '=' - don't match (lexer will use mark_end position from start)
  return false;
}

static bool is_glob_char(int32_t c) {
  return c == '*' || c == '?' || c == '[' || c == ']' || c == '{' || c == '}';
}

static bool is_bare_glob_char(int32_t c) {
  // Characters valid in a glob pattern (not whitespace, colon, or !)
  // ! is excluded because it starts exclusion patterns
  return c != ' ' && c != '\t' && c != '\n' && c != ':' && c != '!' && c != 0 && c != -1;
}

// Scan a bare glob pattern - either contains glob chars, or is an identifier NOT followed by '='
static bool scan_bare_glob(TSLexer *lexer) {
  if (is_space(lexer->lookahead) || lexer->lookahead == ':' || 
      lexer->lookahead == '\n' || lexer->eof(lexer)) {
    return false;
  }
  
  bool has_content = false;
  bool has_glob_char = false;
  bool is_simple_ident = is_option_key_start(lexer->lookahead);
  
  while (is_bare_glob_char(lexer->lookahead) && !lexer->eof(lexer)) {
    if (is_glob_char(lexer->lookahead) || lexer->lookahead == '/') {
      has_glob_char = true;
      is_simple_ident = false;
    }
    if (!is_option_key_char(lexer->lookahead) && lexer->lookahead != '-') {
      is_simple_ident = false;
    }
    advance(lexer);
    has_content = true;
  }
  
  if (!has_content) return false;
  
  // If it looks like a simple identifier, only match if NOT followed by '='
  if (is_simple_ident && lexer->lookahead == '=') {
    return false;
  }
  
  lexer->mark_end(lexer);
  lexer->result_symbol = BARE_GLOB;
  return true;
}

bool tree_sitter_procfile_external_scanner_scan(
  void *payload,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  Scanner *scanner = (Scanner *)payload;

  // Handle line continuation first (highest priority when valid)
  if (valid_symbols[LINE_CONTINUATION] && lexer->lookahead == '\\') {
    lexer->mark_end(lexer);
    if (scan_line_continuation(lexer)) {
      return true;
    }
  }

  // Handle newline
  if (valid_symbols[NEWLINE] && lexer->lookahead == '\n') {
    return scan_newline(lexer, scanner);
  }

  // At start of line (column 0), check for indent/dedent
  if (lexer->get_column(lexer) == 0) {
    if (valid_symbols[DEDENT] && scanner->in_multiline_block) {
      // Peek at indentation
      uint32_t indent = 0;
      while (is_space(lexer->lookahead)) {
        indent++;
        skip(lexer);
      }

      if (lexer->eof(lexer) || (lexer->lookahead != '\n' && indent < scanner->block_indent)) {
        scanner->in_multiline_block = false;
        scanner->block_indent = 0;
        lexer->result_symbol = DEDENT;
        return true;
      }
    }

    if (valid_symbols[INDENT] && !scanner->in_multiline_block) {
      uint32_t indent = 0;
      while (is_space(lexer->lookahead)) {
        indent++;
        skip(lexer);
      }

      if (indent > 0 && lexer->lookahead != '\n' && !lexer->eof(lexer)) {
        scanner->in_multiline_block = true;
        scanner->block_indent = indent;
        lexer->result_symbol = INDENT;
        return true;
      }
    }
  }

  // Handle option key and bare glob together - they need to coordinate
  // because both can start with an identifier
  if (valid_symbols[OPTION_KEY] || valid_symbols[BARE_GLOB]) {
    // Skip whitespace (extras)
    while (is_space(lexer->lookahead)) {
      skip(lexer);
    }
    
    // If it starts like an identifier, we need to look ahead to see if it's
    // followed by '=' (option_key) or not (bare_glob)
    if (is_option_key_start(lexer->lookahead)) {
      // Scan the identifier
      while (is_option_key_char(lexer->lookahead)) {
        advance(lexer);
      }
      
      if (lexer->lookahead == '=' && valid_symbols[OPTION_KEY]) {
        // It's an option key
        lexer->mark_end(lexer);
        lexer->result_symbol = OPTION_KEY;
        return true;
      } else if (valid_symbols[BARE_GLOB]) {
        // It's a bare glob (identifier not followed by =)
        // Continue scanning any remaining glob characters
        while (is_bare_glob_char(lexer->lookahead) && !lexer->eof(lexer)) {
          advance(lexer);
        }
        lexer->mark_end(lexer);
        lexer->result_symbol = BARE_GLOB;
        return true;
      }
    } else if (valid_symbols[BARE_GLOB]) {
      // Doesn't start like an identifier - try bare_glob directly
      if (scan_bare_glob(lexer)) {
        return true;
      }
    }
  }

  // Handle command text (inline command after :)
  if (valid_symbols[COMMAND_TEXT]) {
    // Skip leading whitespace
    while (is_space(lexer->lookahead)) {
      skip(lexer);
    }
    if (lexer->lookahead != '\n' && !lexer->eof(lexer)) {
      return scan_command_text(lexer);
    }
  }

  // Handle multiline command text (inside block)
  if (valid_symbols[MULTILINE_COMMAND_TEXT]) {
    // Skip indentation (already handled by indent tracking)
    while (is_space(lexer->lookahead)) {
      skip(lexer);
    }
    if (lexer->lookahead != '\n' && !lexer->eof(lexer)) {
      return scan_multiline_command_text(lexer);
    }
  }

  return false;
}
