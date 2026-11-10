#include "python_doc_format.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expr, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at line %d\n", expr, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)
bool Has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
void Fits(const std::string &text, int width) {
    int col = 0;
    for (char c : text) {
        if (c == '\n') { CHECK(col <= width); col = 0; }
        else if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) col++;
    }
    CHECK(col <= width);
}
std::string Format(const std::string &text, int width) {
    const auto out = FormatPythonDoc(text, width);
    CHECK(FormatPythonDoc(out, width) == out);
    return out;
}
}  // namespace

int main() {
    auto text = Format("A paragraph that was broken\ninto short source lines should flow naturally across the help window.\n\nAnother paragraph stays separate.", 36);
    CHECK(Has(text, "broken into"));
    CHECK(Has(text, "\n\nAnother paragraph"));
    Fits(text, 36);

    text = Format("Items:\n- The first item has enough text to wrap across several lines and retain its hanging indentation.\n  Its source continuation joins the same item.\n  - A nested bullet also wraps and stays under the parent item correctly.\n- A second item remains separate.\n\n10. A numbered item uses an indentation matching the marker on each continuation.\n11. Another item.", 36);
    CHECK(Has(text, "\n  wrap") || Has(text, "\n  text"));
    CHECK(Has(text, "\n  - A nested bullet"));
    CHECK(Has(text, "\n    under") || Has(text, "\n    stays"));
    CHECK(Has(text, "\n- A second item"));
    CHECK(Has(text, "\n    matching") || Has(text, "\n    indentation"));
    Fits(text, 36);

    text = Format("Parameters\n----------\nvalues : array_like\n    Values supplied to the calculation. This description is indented under its parameter name and wraps independently.\naxis : int, optional\n    Axis over which to calculate.\n\nReturns\n-------\nndarray\n    The computed values remain indented beneath the return type.", 36);
    CHECK(Has(text, "Parameters\n----------\nvalues : array_like\n    Values"));
    CHECK(Has(text, "\naxis : int, optional\n    Axis"));
    CHECK(Has(text, "Returns\n-------\nndarray\n    The"));
    Fits(text, 36);

    text = Format("Args:\n    value (int): A long parameter description that should wrap with the Google continuation indentation.\n    other (str): Another parameter.\n\n:param value: This reST parameter description also uses a hanging indent and keeps the next field separate.\n:returns: The result of the operation.", 36);
    CHECK(Has(text, "    value (int): A long"));
    CHECK(Has(text, "\n        description") || Has(text, "\n        parameter"));
    CHECK(Has(text, "\n    other (str):"));
    CHECK(Has(text, "\n    description also") || Has(text, "\n    parameter description"));
    CHECK(Has(text, "\n:returns:"));
    Fits(text, 36);

    const std::string code = "```python\nvalues = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]\n\nif values:\n    print(values)\n```";
    text = Format("An example.\n\n" + code + "\n\nMore explanatory text that wraps as prose after the example.", 30);
    CHECK(Has(text, code));
    const std::string doctest = ">>> values = [1, 2, 3, 4, 5, 6, 7, 8, 9]\n>>> for value in values:\n...     print(value)\n[1, 2, 3, 4, 5, 6, 7, 8, 9]";
    text = Format("Examples\n--------\n" + doctest + "\n\nThis prose wraps after the doctest.", 30);
    CHECK(Has(text, doctest));
    const std::string literal = "    if ready:\n        print('A long literal line that must stay intact')\n\n\n    print('done')";
    text = Format("Use this example::\n\n" + literal + "\n\nBack to ordinary prose after the literal block.", 30);
    CHECK(Has(text, literal));
    const std::string directive = ".. code-block:: python\n\n    first = 1\n    second = 2\n    print(first, second)";
    text = Format("A code directive.\n\n" + directive + "\n\nProse resumes after the directive.", 30);
    CHECK(Has(text, directive));
    const std::string example_code = "import math\nvalues = [1, 2, 3]\nprint(values)\nprint(math.pi)";
    text = Format("Examples\n--------\n" + example_code + "\n\nExplanatory prose after the code.", 30);
    CHECK(Has(text, example_code));

    text = Format("def calculate(first: int, values=(1, 2), label='one,two', enabled=True) -> str\n\nCalculate something.", 36);
    CHECK(Has(text, "def calculate(\n    first: int,"));
    CHECK(Has(text, "    values=(1, 2),\n    label='one,two',"));
    CHECK(Has(text, ") -> str\n\nCalculate"));
    Fits(text, 36);

    text = Format("# A heading\n\n- [x] A checked item that needs more than one line and must align beneath its text.\n\n> A quoted paragraph that is long enough to need several continuation lines in the same quote.\n\n| Column | Value |\n| ------ | ----- |\n| x      | 42    |", 36);
    CHECK(Has(text, "\n      than") || Has(text, "\n      more"));
    CHECK(Has(text, "\n> "));
    CHECK(Has(text, "| Column | Value |\n| ------ | ----- |\n| x      | 42    |"));
    Fits(text, 36);

    text = Format("Résumé café naïve — Unicode words must be counted in characters and remain valid after wrapping.\n\nhttps://example.invalid/an/extremely/long/identifier/without/spaces", 20);
    CHECK(Has(text, "Résumé café naïve —"));
    Fits(text, 20);
    CHECK(FormatPythonDoc("\r\n\r\n", 72).empty());
    CHECK(FormatPythonDoc("", 72).empty());
    CHECK(FormatPythonDoc("one two three", 0) == "one two\nthree");
    CHECK(PythonDocColumns(72, 0, 0.0f) == 72);
    CHECK(PythonDocColumns(72, 1600, 12.0f) == 72);
    CHECK(PythonDocColumns(72, 600, 12.0f) == 45);
    CHECK(PythonDocColumns(40, 1600, 12.0f) == 40);
    CHECK(PythonDocColumns(72, 800, 24.0f) == 30);
    std::puts("Python doc formatting checks passed");
}
