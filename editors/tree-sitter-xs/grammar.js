const PREC = {
  pipe:       1,
  or:         2,
  and:        3,
  not:        4,
  compare:    5,
  bit_or:     6,
  bit_xor:    7,
  bit_and:    8,
  shift:      9,
  additive:  10,
  multiplic: 11,
  power:     12,
  unary:     13,
  nullcoal:  14,
  call:      15,
  field:     16,
  paren:     17,
};

module.exports = grammar({
  name: 'xs',

  extras: $ => [
    /\s/,
    $.line_comment,
    $.block_comment,
  ],

  word: $ => $.identifier,

  conflicts: $ => [
    [$.type_identifier, $.identifier],
    [$.call_expression, $.field_expression],
    [$._statement, $._expression],
    [$.block, $.map_literal],
  ],

  rules: {
    source_file: $ => repeat($._statement),

    // ---- comments --------------------------------------------------------

    line_comment:  $ => token(seq('--', /.*/)),
    block_comment: $ => token(seq('{-', /[^-]*(-+[^-}][^-]*)*/ , '-}')),

    // ---- top-level statements -------------------------------------------

    _statement: $ => choice(
      $.import_statement,
      $.use_statement,
      $.export_statement,
      $.let_declaration,
      $.var_declaration,
      $.const_declaration,
      $.function_declaration,
      $.struct_declaration,
      $.enum_declaration,
      $.trait_declaration,
      $.impl_declaration,
      $.class_declaration,
      $.type_alias,
      $.effect_declaration,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.throw_statement,
      $.defer_statement,
      $.yield_statement,
      $.if_statement,
      $.while_statement,
      $.for_statement,
      $.loop_statement,
      $.match_statement,
      $.try_statement,
      $.block,
      $._expression_statement,
    ),

    _expression_statement: $ => seq($._expression, optional(';')),

    // ---- imports ---------------------------------------------------------

    import_statement: $ => seq(
      'import',
      choice(
        $.identifier,
        seq($.identifier, 'as', $.identifier),
        seq('{', commaSep1($.identifier), '}', 'from', $.string_literal),
      ),
    ),

    use_statement: $ => seq(
      'use',
      optional('plugin'),
      choice($.string_literal, $.identifier),
      optional(seq('as', $.identifier)),
    ),

    export_statement: $ => seq('export', $._statement),

    // ---- bindings --------------------------------------------------------

    let_declaration: $ => seq(
      'let',
      field('name', $._pattern_or_ident),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('value', $._expression))),
    ),

    var_declaration: $ => seq(
      'var',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('value', $._expression))),
    ),

    const_declaration: $ => seq(
      'const',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      '=',
      field('value', $._expression),
    ),

    _pattern_or_ident: $ => choice(
      $.identifier,
      $.tuple_pattern,
      $.array_pattern,
      $.struct_pattern,
    ),

    // ---- functions -------------------------------------------------------

    function_declaration: $ => seq(
      choice('fn', 'fn*'),
      field('name', $.identifier),
      optional($.generic_params),
      field('parameters', $.parameter_list),
      optional(seq('->', field('return_type', $._type))),
      optional($.where_clause),
      field('body', choice($.block, seq('=', $._expression))),
    ),

    parameter_list: $ => seq(
      '(',
      optional(commaSep1($.parameter)),
      ')',
    ),

    parameter: $ => seq(
      optional('...'),
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('default', $._expression))),
    ),

    generic_params: $ => seq('<', commaSep1($.identifier), '>'),

    where_clause: $ => seq('where', commaSep1($._expression)),

    // ---- types -----------------------------------------------------------

    struct_declaration: $ => seq(
      'struct',
      field('name', $.identifier),
      optional($.generic_params),
      '{',
      commaSep($.struct_field),
      optional(','),
      '}',
    ),

    struct_field: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $._type),
    ),

    enum_declaration: $ => seq(
      'enum',
      field('name', $.identifier),
      optional($.generic_params),
      '{',
      commaSep($.enum_variant),
      optional(','),
      '}',
    ),

    enum_variant: $ => seq(
      field('name', $.identifier),
      optional(seq('(', commaSep1($._type), ')')),
    ),

    trait_declaration: $ => seq(
      'trait',
      field('name', $.identifier),
      optional($.generic_params),
      '{',
      repeat(choice($.function_declaration, $.function_signature)),
      '}',
    ),

    function_signature: $ => seq(
      'fn',
      field('name', $.identifier),
      optional($.generic_params),
      $.parameter_list,
      optional(seq('->', $._type)),
    ),

    impl_declaration: $ => seq(
      'impl',
      optional($.generic_params),
      field('type', $._type),
      optional(seq('for', field('target', $._type))),
      '{',
      repeat($.function_declaration),
      '}',
    ),

    class_declaration: $ => seq(
      'class',
      field('name', $.identifier),
      optional($.generic_params),
      optional(seq(':', commaSep1($._type))),
      '{',
      repeat(choice(
        $.function_declaration,
        $.struct_field,
      )),
      '}',
    ),

    type_alias: $ => seq(
      'type',
      field('name', $.identifier),
      optional($.generic_params),
      '=',
      field('type', $._type),
    ),

    effect_declaration: $ => seq(
      'effect',
      field('name', $.identifier),
      '{',
      repeat($.function_signature),
      '}',
    ),

    _type: $ => choice(
      $.primitive_type,
      $.type_identifier,
      $.generic_type,
      $.function_type,
      $.tuple_type,
      $.array_type,
      $.option_type,
      $.reference_type,
    ),

    primitive_type: $ => choice(
      'int', 'i8', 'i16', 'i32', 'i64',
      'u8', 'u16', 'u32', 'u64',
      'float', 'f32', 'f64',
      'str', 'string', 'bool', 'char', 'byte', 're',
      'any', 'dyn', 'void', 'unit', 'never',
    ),

    type_identifier: $ => /[A-Z][a-zA-Z0-9_]*/,

    generic_type: $ => seq(
      $.type_identifier,
      '<',
      commaSep1($._type),
      '>',
    ),

    function_type: $ => seq(
      'fn',
      '(',
      commaSep($._type),
      ')',
      optional(seq('->', $._type)),
    ),

    tuple_type:     $ => seq('(', commaSep1($._type), ')'),
    array_type:     $ => seq('[', $._type, optional(seq(';', $.number_literal)), ']'),
    option_type:    $ => seq($._type, '?'),
    reference_type: $ => seq('&', optional('mut'), $._type),

    // ---- statements ------------------------------------------------------

    return_statement:   $ => prec.right(seq('return', optional($._expression))),
    break_statement:    $ => prec.right(seq('break', optional($.identifier), optional($._expression))),
    continue_statement: $ => prec.right(seq('continue', optional($.identifier))),
    throw_statement:    $ => seq('throw', $._expression),
    defer_statement:    $ => seq('defer', $._expression),
    yield_statement:    $ => prec.right(seq('yield', optional($._expression))),

    if_statement: $ => prec.right(seq(
      'if',
      field('condition', $._expression),
      field('consequence', $.block),
      repeat(seq('elif', $._expression, $.block)),
      optional(seq('else', choice($.block, $.if_statement))),
    )),

    while_statement: $ => seq(
      optional(seq(field('label', $.identifier), ':')),
      'while',
      field('condition', $._expression),
      field('body', $.block),
    ),

    for_statement: $ => seq(
      optional(seq(field('label', $.identifier), ':')),
      'for',
      field('name', $._pattern_or_ident),
      'in',
      field('iter', $._expression),
      optional($.where_clause),
      field('body', $.block),
    ),

    loop_statement: $ => seq(
      optional(seq(field('label', $.identifier), ':')),
      'loop',
      field('body', $.block),
    ),

    match_statement: $ => seq(
      'match',
      field('scrutinee', $._expression),
      '{',
      repeat($.match_arm),
      '}',
    ),

    match_arm: $ => seq(
      field('pattern', $._pattern),
      optional(seq('when', field('guard', $._expression))),
      '->',
      field('body', choice($._expression, $.block)),
      optional(','),
    ),

    try_statement: $ => seq(
      'try',
      field('body', $.block),
      repeat($.catch_clause),
      optional($.finally_clause),
    ),

    catch_clause:   $ => seq('catch',   optional($.identifier), $.block),
    finally_clause: $ => seq('finally', $.block),

    block: $ => seq('{', repeat($._statement), '}'),

    // ---- patterns --------------------------------------------------------

    _pattern: $ => choice(
      $.wildcard_pattern,
      $.literal_pattern,
      $.identifier,
      $.tuple_pattern,
      $.array_pattern,
      $.struct_pattern,
      $.enum_pattern,
      $.range_pattern,
    ),

    wildcard_pattern: $ => '_',
    literal_pattern:  $ => choice($.number_literal, $.string_literal, $.boolean_literal, $.null_literal),
    tuple_pattern:    $ => seq('(', commaSep1($._pattern), ')'),
    array_pattern:    $ => seq('[', commaSep($._pattern), ']'),
    struct_pattern:   $ => seq($.type_identifier, '{', commaSep($._pattern), '}'),
    enum_pattern:     $ => seq($.type_identifier, optional(seq('(', commaSep($._pattern), ')'))),
    range_pattern:    $ => seq($.literal_pattern, choice('..', '..='), $.literal_pattern),

    // ---- expressions -----------------------------------------------------

    _expression: $ => choice(
      $.number_literal,
      $.string_literal,
      $.char_literal,
      $.boolean_literal,
      $.null_literal,
      $.universal_literal,
      $.identifier,
      $.self_expression,
      $.array_literal,
      $.map_literal,
      $.tuple_literal,
      $.range_expression,
      $.unary_expression,
      $.binary_expression,
      $.call_expression,
      $.field_expression,
      $.index_expression,
      $.lambda_expression,
      $.if_expression,
      $.match_expression,
      $.block,
      $.parenthesized,
      $.spawn_expression,
      $.await_expression,
      $.async_expression,
      $.perform_expression,
      $.handle_expression,
      $.resume_expression,
      $.pipe_expression,
      $.assignment_expression,
    ),

    parenthesized: $ => seq('(', $._expression, ')'),

    self_expression: $ => choice('self', 'super'),

    array_literal: $ => seq('[', commaSep($._expression), optional(','), ']'),
    map_literal:   $ => seq('{', commaSep($.map_entry), optional(','), '}'),
    map_entry:     $ => seq($._expression, ':', $._expression),
    tuple_literal: $ => seq('(', $._expression, ',', commaSep($._expression), ')'),

    range_expression: $ => prec.left(5, seq($._expression, choice('..', '..='), $._expression)),

    unary_expression: $ => prec(PREC.unary, seq(
      field('op', choice('-', '!', 'not', '~')),
      field('operand', $._expression),
    )),

    binary_expression: $ => choice(
      prec.left(PREC.or,        seq($._expression, field('op', choice('||', 'or')),  $._expression)),
      prec.left(PREC.and,       seq($._expression, field('op', choice('&&', 'and')), $._expression)),
      prec.left(PREC.compare,   seq($._expression, field('op', choice('==', '!=', '<', '<=', '>', '>=', 'is', 'in')), $._expression)),
      prec.left(PREC.bit_or,    seq($._expression, field('op', '|'),  $._expression)),
      prec.left(PREC.bit_xor,   seq($._expression, field('op', '^'),  $._expression)),
      prec.left(PREC.bit_and,   seq($._expression, field('op', '&'),  $._expression)),
      prec.left(PREC.shift,     seq($._expression, field('op', choice('<<', '>>')), $._expression)),
      prec.left(PREC.additive,  seq($._expression, field('op', choice('+', '-')),    $._expression)),
      prec.left(PREC.multiplic, seq($._expression, field('op', choice('*', '/', '%')), $._expression)),
      prec.right(PREC.power,    seq($._expression, field('op', '**'), $._expression)),
      prec.right(PREC.nullcoal, seq($._expression, field('op', '??'), $._expression)),
    ),

    assignment_expression: $ => prec.right(0, seq(
      field('target', $._expression),
      field('op',     choice('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=')),
      field('value',  $._expression),
    )),

    call_expression: $ => prec(PREC.call, seq(
      field('callee', $._expression),
      '(',
      commaSep($._expression),
      ')',
    )),

    field_expression: $ => prec.left(PREC.field, seq(
      field('object', $._expression),
      '.',
      field('field', $.identifier),
    )),

    index_expression: $ => prec(PREC.field, seq(
      field('object', $._expression),
      '[',
      field('index', $._expression),
      ']',
    )),

    lambda_expression: $ => seq(
      '|',
      commaSep($.parameter),
      '|',
      choice($._expression, $.block),
    ),

    if_expression: $ => seq(
      'if',
      field('condition', $._expression),
      field('consequence', $.block),
      repeat(seq('elif', $._expression, $.block)),
      'else',
      field('alternative', choice($.block, $.if_expression)),
    ),

    match_expression: $ => seq(
      'match',
      field('scrutinee', $._expression),
      '{',
      repeat($.match_arm),
      '}',
    ),

    spawn_expression:   $ => prec(PREC.unary, seq('spawn',   $._expression)),
    await_expression:   $ => prec(PREC.unary, seq('await',   $._expression)),
    async_expression:   $ => prec(PREC.unary, seq('async',   $._expression)),
    perform_expression: $ => prec(PREC.unary, seq('perform', $._expression)),
    resume_expression:  $ => prec.right(PREC.unary, seq('resume', optional($._expression))),
    handle_expression:  $ => seq(
      'handle',
      $._expression,
      '{',
      repeat($.match_arm),
      '}',
    ),

    pipe_expression: $ => prec.left(PREC.pipe, seq($._expression, '|>', $._expression)),

    // ---- literals --------------------------------------------------------

    number_literal: $ => token(choice(
      /0x[0-9a-fA-F_]+/,
      /0o[0-7_]+/,
      /0b[01_]+/,
      /[0-9][0-9_]*(\.[0-9][0-9_]*)?([eE][+-]?[0-9_]+)?/,
    )),

    string_literal: $ => choice(
      $._triple_string,
      $._double_string,
      $._raw_string,
    ),

    _triple_string: $ => seq('"""', repeat(choice(/[^"\\]/, /\\./, '"', '""')), '"""'),
    _double_string: $ => seq('"',   repeat(choice(/[^"\\]/, /\\./)),            '"'),
    _raw_string:    $ => seq('r"',  repeat(/[^"]/),                             '"'),

    char_literal:    $ => token(seq("'", /([^'\\]|\\.)/, "'")),
    boolean_literal: $ => choice('true', 'false'),
    null_literal:    $ => 'null',

    /* Universal literals: 500ms, #ff6600, 2025-01-20, 10MB, 45deg */
    universal_literal: $ => token(choice(
      /[0-9]+(\.[0-9]+)?(ns|us|ms|s|min|h|d|w|mo|y)/,
      /#[0-9a-fA-F]{3,8}/,
      /[0-9]+-[0-9]{2}-[0-9]{2}(T[0-9]{2}:[0-9]{2}(:[0-9]{2})?)?/,
      /[0-9]+(\.[0-9]+)?(B|KB|MB|GB|TB|KiB|MiB|GiB|TiB)/,
      /[0-9]+(\.[0-9]+)?(deg|rad|turn|grad)/,
    )),

    identifier: $ => /[a-z_][a-zA-Z0-9_]*/,
  },
});

function commaSep(rule)  { return optional(commaSep1(rule)); }
function commaSep1(rule) { return seq(rule, repeat(seq(',', rule))); }
