/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

module.exports = grammar({
  name: 'procfile',

  externals: $ => [
    $._newline,
    $._indent,
    $._dedent,
    $._command_text,
    $._multiline_command_text,
    $.line_continuation,
    $.option_key,    // scanned with lookahead for '='
    $.bare_glob,     // identifier NOT followed by '=' or glob pattern
  ],

  extras: $ => [
    / +/,  // horizontal whitespace only (not newlines)
    $.line_continuation,
  ],

  rules: {
    source_file: $ => repeat($._definition),

    _definition: $ => choice(
      $.comment,
      $.process_definition,
      $._newline,  // blank lines
    ),

    comment: $ => seq('#', /[^\n]*/),

    process_definition: $ => seq(
      $.declaration,
      ':',
      optional($.execution),
      $._newline,
      optional($.multiline_block),
    ),

    declaration: $ => seq(
      $.process_name,
      repeat($._declaration_item),
    ),

    process_name: $ => token(seq(
      /[a-zA-Z_][a-zA-Z0-9_-]*/,
      optional('!'),
    )),

    _declaration_item: $ => choice(
      $.option,
      $.glob_pattern,
      $.exclusion_pattern,
    ),

    // Options: key=value where key is a simple identifier
    // option_key is an external token that only matches if followed by '='
    option: $ => seq(
      field('key', $.option_key),
      '=',
      field('value', $.option_value),
    ),

    option_value: $ => choice(
      $._quoted_string,
      $._bare_option_value,
    ),

    _bare_option_value: $ => /[^\s:]+/,

    glob_pattern: $ => choice(
      $._quoted_string,
      $.bare_glob,  // external: matches glob patterns and bare filenames NOT followed by '='
    ),

    exclusion_pattern: $ => seq(
      '!',
      choice(
        $._quoted_string,
        $.bare_glob,
      ),
    ),

    execution: $ => choice(
      seq(repeat1($.env_var), optional($.command)),
      $.command,
    ),

    env_var: $ => seq(
      $.env_key,
      '=',
      $.env_value,
    ),

    env_key: $ => /[A-Z_][A-Z0-9_]*/,

    env_value: $ => choice(
      $._quoted_string,
      $._bare_env_value,
    ),

    _bare_env_value: $ => /[^\s]+/,

    command: $ => $._command_text,

    multiline_block: $ => seq(
      $._indent,
      repeat1($.block_line),
      $._dedent,
    ),

    block_line: $ => seq(
      $._multiline_command_text,
      $._newline,
    ),

    _quoted_string: $ => choice(
      $.single_quoted_string,
      $.double_quoted_string,
    ),

    single_quoted_string: $ => seq(
      "'",
      /[^']*/,
      "'",
    ),

    double_quoted_string: $ => seq(
      '"',
      repeat(choice(
        /[^"\\]+/,
        $.escape_sequence,
      )),
      '"',
    ),

    escape_sequence: $ => /\\./,
  },
});
