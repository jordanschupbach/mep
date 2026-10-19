; A recipe's body is shell unless a shebang names another interpreter.
((recipe_body) @injection.content
  (#not-match? @injection.content "^\\s*#!")
  (#set! injection.language "bash")
  (#set! injection.include-children))

((backtick) @injection.content
  (#set! injection.language "bash"))
