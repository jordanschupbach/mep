#pragma once

// Unicode character names and a few common search aliases for the symbol
// picker. The codepoints also drive insertion, independent of the UI font.
constexpr const char *kBuiltinSymbolPicker = R"MEPSYM(
local groups = {
  { 'Greek', [[
03B1|greek small letter alpha|alpha
03B2|greek small letter beta|beta
03B3|greek small letter gamma|gamma
03B4|greek small letter delta|delta
03B5|greek small letter epsilon|epsilon
03B6|greek small letter zeta|zeta
03B7|greek small letter eta|eta
03B8|greek small letter theta|theta
03B9|greek small letter iota|iota
03BA|greek small letter kappa|kappa
03BB|greek small letter lamda|lambda
03BC|greek small letter mu|mu
03BD|greek small letter nu|nu
03BE|greek small letter xi|xi
03BF|greek small letter omicron|omicron
03C0|greek small letter pi|pi
03C1|greek small letter rho|rho
03C2|greek small letter final sigma|final sigma
03C3|greek small letter sigma|sigma
03C4|greek small letter tau|tau
03C5|greek small letter upsilon|upsilon
03C6|greek small letter phi|phi
03C7|greek small letter chi|chi
03C8|greek small letter psi|psi
03C9|greek small letter omega|omega
0391|greek capital letter alpha|capital alpha
0392|greek capital letter beta|capital beta
0393|greek capital letter gamma|capital gamma
0394|greek capital letter delta|capital delta
0395|greek capital letter epsilon|capital epsilon
0396|greek capital letter zeta|capital zeta
0397|greek capital letter eta|capital eta
0398|greek capital letter theta|capital theta
0399|greek capital letter iota|capital iota
039A|greek capital letter kappa|capital kappa
039B|greek capital letter lamda|capital lambda
039C|greek capital letter mu|capital mu
039D|greek capital letter nu|capital nu
039E|greek capital letter xi|capital xi
039F|greek capital letter omicron|capital omicron
03A0|greek capital letter pi|capital pi
03A1|greek capital letter rho|capital rho
03A3|greek capital letter sigma|capital sigma
03A4|greek capital letter tau|capital tau
03A5|greek capital letter upsilon|capital upsilon
03A6|greek capital letter phi|capital phi
03A7|greek capital letter chi|capital chi
03A8|greek capital letter psi|capital psi
03A9|greek capital letter omega|capital omega
03D1|greek theta symbol
03D5|greek phi symbol
03D6|greek pi symbol
03F0|greek kappa symbol
03F1|greek rho symbol
03F5|greek lunate epsilon symbol
  ]] },
  { 'Arrows', [[
2190|leftwards arrow|left arrow leftarrow gets
2191|upwards arrow|up arrow uparrow
2192|rightwards arrow|right arrow rightarrow to
2193|downwards arrow|down arrow downarrow
2194|left right arrow|left right arrow leftrightarrow
2195|up down arrow|up down arrow updownarrow
2196|north west arrow
2197|north east arrow
2198|south east arrow
2199|south west arrow
21A6|rightwards arrow from bar|maps to mapsto
21A9|leftwards arrow with hook
21AA|rightwards arrow with hook
21B5|downwards arrow with corner leftwards|return enter
21BA|anticlockwise open circle arrow|counterclockwise undo
21BB|clockwise open circle arrow|clockwise refresh
21CC|rightwards harpoon over leftwards harpoon|equilibrium reversible reaction
21D0|leftwards double arrow|double left arrow impliedby
21D2|rightwards double arrow|double right arrow implies implication
21D4|left right double arrow|double left right arrow iff if and only if
27F5|long leftwards arrow
27F6|long rightwards arrow
27F7|long left right arrow
  ]] },
  { 'Mathematics', [[
2102|double-struck capital c|complex numbers mathbb c
210D|double-struck capital h
2113|script small l
2115|double-struck capital n|natural numbers mathbb n
2119|double-struck capital p|probability mathbb p
211A|double-struck capital q|rational numbers mathbb q
211D|double-struck capital r|real numbers mathbb r
2124|double-struck capital z|integers mathbb z
2200|for all|for all forall universal quantifier
2201|complement
2202|partial differential|partial derivative
2203|there exists|exists existential quantifier
2205|empty set|empty set null set emptyset varnothing
2207|nabla|nabla gradient del
2208|element of|element of in member
2209|not an element of|not in notin
220B|contains as member
220E|end of proof
220F|n-ary product|product prod
2210|n-ary coproduct
2211|n-ary summation|sum summation
2212|minus sign
2213|minus-or-plus sign|minus plus mp
2214|dot plus
2218|ring operator
2219|bullet operator
221A|square root|square root sqrt radical
221B|cube root
221C|fourth root
221E|infinity|infinity infty
2223|divides
2224|does not divide
2225|parallel to
2227|logical and
2228|logical or
2229|intersection|intersection cap
222A|union|union cup
222B|integral|integral int
222C|double integral
222D|triple integral
222E|contour integral
2234|therefore
2235|because
223C|tilde operator|similar tilde sim
2243|asymptotically equal to
2245|approximately equal to
2248|almost equal to|approximately equal approx
224D|equivalent to
2254|colon equals
2260|not equal to|not equal ne neq
2261|identical to|equivalent equiv
2264|less-than or equal to|less than or equal le leq
2265|greater-than or equal to|greater than or equal ge geq
226A|much less-than
226B|much greater-than
227A|precedes
227B|succeeds
2282|subset of
2283|superset of
2286|subset of or equal to|subset or equal subseteq
2287|superset of or equal to|superset or equal supseteq
2288|neither a subset of nor equal to
228E|multiset union
2291|square image of or equal to
2292|square original of or equal to
2293|square cap
2294|square cup
2295|circled plus
2296|circled minus
2297|circled times
2298|circled division slash
2299|circled dot operator
22A2|right tack
22A3|left tack
22A4|down tack
22A5|up tack|perpendicular perp bottom
22C2|n-ary intersection
22C3|n-ary union
22C4|diamond operator
22C6|star operator
22EE|vertical ellipsis
22EF|midline horizontal ellipsis
22F1|down right diagonal ellipsis
2308|left ceiling
2309|right ceiling
230A|left floor
230B|right floor
27E8|mathematical left angle bracket
27E9|mathematical right angle bracket
00B1|plus-minus sign|plus minus pm
00D7|multiplication sign|times multiplication
00F7|division sign|division divide
  ]] },
  { 'Superscripts and subscripts', [[
00B9|superscript one
00B2|superscript two
00B3|superscript three
2070|superscript zero
2071|superscript latin small letter i
2074|superscript four
2075|superscript five
2076|superscript six
2077|superscript seven
2078|superscript eight
2079|superscript nine
207A|superscript plus sign
207B|superscript minus
207C|superscript equals sign
207D|superscript left parenthesis
207E|superscript right parenthesis
207F|superscript latin small letter n
2080|subscript zero
2081|subscript one
2082|subscript two
2083|subscript three
2084|subscript four
2085|subscript five
2086|subscript six
2087|subscript seven
2088|subscript eight
2089|subscript nine
208A|subscript plus sign
208B|subscript minus
208C|subscript equals sign
208D|subscript left parenthesis
208E|subscript right parenthesis
2090|latin subscript small letter a
2091|latin subscript small letter e
2092|latin subscript small letter o
2093|latin subscript small letter x
2094|latin subscript small letter schwa
2095|latin subscript small letter h
2096|latin subscript small letter k
2097|latin subscript small letter l
2098|latin subscript small letter m
2099|latin subscript small letter n
209A|latin subscript small letter p
209B|latin subscript small letter s
209C|latin subscript small letter t
  ]] },
  { 'Typography and currency', [[
2014|em dash|em dash long dash
2013|en dash|en dash range dash
2026|horizontal ellipsis|ellipsis dots ldots
2022|bullet|bullet dot
00B7|middle dot
2018|left single quotation mark
2019|right single quotation mark
201C|left double quotation mark
201D|right double quotation mark
00AB|left-pointing double angle quotation mark
00BB|right-pointing double angle quotation mark
2039|single left-pointing angle quotation mark
203A|single right-pointing angle quotation mark
2032|prime
2033|double prime
2020|dagger
2021|double dagger
00A7|section sign
00B6|pilcrow sign
00A9|copyright sign|copyright
00AE|registered sign|registered trademark
2122|trade mark sign|trade mark trademark
00B0|degree sign|degree temperature
00B5|micro sign|micro mu
00BA|masculine ordinal indicator|ordinal
20AC|euro sign
00A3|pound sign
00A5|yen sign
00A2|cent sign
20B9|indian rupee sign
20BF|bitcoin sign
  ]] },
  { 'Marks and shapes', [[
2713|check mark|checkmark tick correct
2714|heavy check mark|heavy checkmark tick
2717|ballot x|cross wrong x mark
2718|heavy ballot x
2605|black star|star filled
2606|white star|star outline
2665|black heart suit|heart suit
2666|black diamond suit
2663|black club suit
2660|black spade suit
266A|eighth note
266B|beamed eighth notes
266D|music flat sign|music flat
266F|music sharp sign|music sharp
25CB|white circle
25CF|black circle
25A1|white square
25A0|black square
25C7|white diamond
25C6|black diamond
25B3|white up-pointing triangle
25B2|black up-pointing triangle
25BD|white down-pointing triangle
25BC|black down-pointing triangle
  ]] },
}
local items, symbols = {}, {}
for _, group in ipairs(groups) do
  for row in group[2]:gmatch('[^\n]+') do
    local hex, name, aliases = row:match('^(%x+)|([^|]+)|?(.*)$')
    if hex then
      local glyph = utf8.char(tonumber(hex, 16))
      local code = 'U+' .. hex
      local symbol = { glyph = glyph, name = name, aliases = aliases, code = code, group = group[1] }
      symbols[glyph] = symbol
      local display = glyph .. '  ' .. name .. '  ' .. code .. '  ' .. group[1]
      if aliases ~= '' then display = display .. '  ' .. aliases end
      items[#items + 1] = { display = display, data = glyph, key = aliases ~= '' and aliases or name }
    end
  end
end

function mep.insert_symbol()
  if not mep.can_insert_text() then
    mep.notify('Choose an editable text buffer to insert a symbol', 'warn')
    return
  end
  local buffer, pane = mep.current_buffer(), mep.current_pane_id()
  local function preview(glyph)
    local symbol = symbols[glyph]
    if not symbol then mep.picker_set_preview('') return end
    local lines = {symbol.name, symbol.code .. '  ' .. symbol.group}
    if symbol.aliases ~= '' then lines[#lines + 1] = symbol.aliases end
    mep.picker_set_preview(table.concat(lines, '\n'), nil, glyph)
  end
  mep.picker_open('Insert symbol', items, function(glyph)
    if not glyph then return end
    if mep.current_buffer() == buffer and mep.current_pane_id() == pane and mep.can_insert_text() then
      mep.insert_text(glyph)
    end
  end, function() preview(mep.picker_selected_data()) end, nil, preview)
  preview(items[1].data)
end
)MEPSYM";
