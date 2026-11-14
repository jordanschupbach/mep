// Ruby's core classes and methods, as far as mep's Ruby language server knows
// them without running Ruby: names, parameters, return types and a line of
// documentation each (ruby_lsp_internal.h's Builtins()). Return types may use
// `$self` (the receiver's type) and `$elem` (its element type: `Array<Frame>`
// -> `Frame`), which the type inference in ruby_lsp.cpp fills in.
#include <mutex>

#include "ruby_lsp_internal.h"

namespace rblsp {

namespace {

// kind: 'c' class (params = superclass, ret = included modules), 'm' module,
// 'i' instance method, 's' singleton method, 'k' constant.
struct Entry {
    char kind;
    const char *owner;
    const char *name;
    const char *params;
    const char *ret;
    const char *doc;
};

const Entry kEntries[] = {
    {'c', "", "BasicObject", "", "", "The root of Ruby's class hierarchy."},
    {'m', "", "Kernel", "", "", "Methods available to every object: `puts`, `require`, `raise`, ..."},
    {'c', "", "Object", "BasicObject", "Kernel", "The default superclass of every class."},
    {'c', "", "Module", "Object", "", "A collection of methods and constants."},
    {'c', "", "Class", "Module", "", "A class: a module that can be instantiated."},
    {'m', "", "Comparable", "", "", "Comparison operators from `<=>`."},
    {'m', "", "Enumerable", "", "", "Collection methods built on `each`."},
    {'c', "", "Numeric", "Object", "Comparable", "The superclass of all numbers."},
    {'c', "", "Integer", "Numeric", "", "Whole numbers of any size."},
    {'c', "", "Float", "Numeric", "", "Double-precision floating point numbers."},
    {'c', "", "String", "Object", "Comparable", "A sequence of bytes in some encoding."},
    {'c', "", "Symbol", "Object", "Comparable", "An interned name: `:name`."},
    {'c', "", "Array", "Object", "Enumerable", "An ordered, integer-indexed collection."},
    {'c', "", "Hash", "Object", "Enumerable", "A dictionary of unique keys and their values."},
    {'c', "", "Range", "Object", "Enumerable", "An interval: `1..10`, `'a'...'z'`."},
    {'c', "", "NilClass", "Object", "", "The class of `nil`."},
    {'c', "", "TrueClass", "Object", "", "The class of `true`."},
    {'c', "", "FalseClass", "Object", "", "The class of `false`."},
    {'c', "", "Proc", "Object", "", "A block of code bound to its context: `proc { }`, `-> { }`."},
    {'c', "", "Regexp", "Object", "", "A regular expression: `/pattern/`."},
    {'c', "", "MatchData", "Object", "", "The result of a successful regular expression match."},
    {'c', "", "Struct", "Object", "Enumerable", "A class generator for simple data holders."},
    {'c', "", "Time", "Object", "Comparable", "A point in time."},
    {'c', "", "IO", "Object", "Enumerable", "Input and output streams."},
    {'c', "", "File", "IO", "", "A file on disk."},
    {'c', "", "Dir", "Object", "Enumerable", "Directory streams and listings."},
    {'m', "", "Math", "", "", "Basic trigonometric and transcendental functions."},
    {'c', "", "Exception", "Object", "", "The root of the exception hierarchy."},
    {'c', "", "StandardError", "Exception", "", "The default class rescued by a bare `rescue`."},
    {'c', "", "ArgumentError", "StandardError", "", "Raised for wrong arguments."},
    {'c', "", "RuntimeError", "StandardError", "", "The default class of `raise \"message\"`."},
    {'c', "", "TypeError", "StandardError", "", "Raised when an object has the wrong type."},
    {'c', "", "NameError", "StandardError", "", "Raised for an undefined name."},
    {'c', "", "NoMethodError", "NameError", "", "Raised for an undefined method."},
    {'c', "", "IndexError", "StandardError", "", "Raised for an index out of range."},
    {'c', "", "KeyError", "IndexError", "", "Raised by `fetch` for a missing key."},
    {'c', "", "RangeError", "StandardError", "", "Raised for a numeric value out of range."},
    {'c', "", "ZeroDivisionError", "StandardError", "", "Raised by integer division by zero."},
    {'c', "", "IOError", "StandardError", "", "Raised when an IO operation fails."},
    {'c', "", "NotImplementedError", "StandardError", "", "Raised for a feature that is not available."},
    {'c', "", "StopIteration", "IndexError", "", "Raised by `next` at the end of an enumerator."},

    {'k', "Math", "PI", "", "Float", "π, 3.141592653589793."},
    {'k', "Math", "E", "", "Float", "e, 2.718281828459045."},
    {'k', "Float", "INFINITY", "", "Float", "Positive infinity."},
    {'k', "Float", "NAN", "", "Float", "Not a number."},
    {'k', "Float", "EPSILON", "", "Float", "The difference between 1.0 and the next larger Float."},
    {'k', "", "ARGV", "", "Array<String>", "The command-line arguments."},
    {'k', "", "ENV", "", "Hash", "The environment variables."},
    {'k', "", "STDOUT", "", "IO", "Standard output."},
    {'k', "", "STDERR", "", "IO", "Standard error."},
    {'k', "", "STDIN", "", "IO", "Standard input."},
    {'k', "", "RUBY_VERSION", "", "String", "The Ruby version, `\"3.4.0\"`."},

    // Kernel
    {'i', "Kernel", "puts", "(*objects)", "nil", "Writes each object and a newline to standard output."},
    {'i', "Kernel", "print", "(*objects)", "nil", "Writes the objects to standard output, without newlines."},
    {'i', "Kernel", "p", "(*objects)", "$self", "Writes `inspect` of each object; returns its argument."},
    {'i', "Kernel", "pp", "(*objects)", "$self", "Pretty-prints the objects."},
    {'i', "Kernel", "require", "(name)", "Boolean", "Loads library `name` from the load path, once."},
    {'i', "Kernel", "require_relative", "(path)", "Boolean", "Loads a file relative to this file, once."},
    {'i', "Kernel", "load", "(path, wrap = false)", "Boolean", "Loads and runs a Ruby file every time."},
    {'i', "Kernel", "raise", "(exception = RuntimeError, message = nil, backtrace = nil, cause: $!)", "",
     "Raises an exception."},
    {'i', "Kernel", "format", "(format_string, *args)", "String", "Formats arguments like C's `sprintf`."},
    {'i', "Kernel", "sprintf", "(format_string, *args)", "String", "Formats arguments like C's `sprintf`."},
    {'i', "Kernel", "printf", "(format_string, *args)", "nil", "Prints formatted output."},
    {'i', "Kernel", "gets", "(sep = $/, chomp: false)", "String", "Reads a line from standard input."},
    {'i', "Kernel", "rand", "(max = 0)", "Numeric", "A random number: a Float in [0, 1), or an Integer below `max`."},
    {'i', "Kernel", "srand", "(seed = Random.new_seed)", "Integer", "Seeds the random number generator."},
    {'i', "Kernel", "sleep", "(seconds = nil)", "Integer", "Suspends the thread for `seconds`."},
    {'i', "Kernel", "loop", "(&block)", "", "Runs the block forever (until `break` or StopIteration)."},
    {'i', "Kernel", "lambda", "(&block)", "Proc", "A lambda from the block."},
    {'i', "Kernel", "proc", "(&block)", "Proc", "A proc from the block."},
    {'i', "Kernel", "block_given?", "", "Boolean", "Whether the current method was called with a block."},
    {'i', "Kernel", "catch", "(tag = Object.new, &block)", "", "Runs the block; `throw tag` returns from it."},
    {'i', "Kernel", "throw", "(tag, value = nil)", "", "Unwinds to the matching `catch`."},
    {'i', "Kernel", "exit", "(status = true)", "", "Exits the process."},
    {'i', "Kernel", "abort", "(message = nil)", "", "Exits with status 1, printing `message`."},
    {'i', "Kernel", "at_exit", "(&block)", "Proc", "Runs the block when the program exits."},
    {'i', "Kernel", "binding", "", "Binding", "The current binding."},
    {'i', "Kernel", "caller", "(start = 1, length = nil)", "Array<String>", "The current call stack."},
    {'i', "Kernel", "Integer", "(arg, base = 0, exception: true)", "Integer", "Converts `arg` to an Integer, strictly."},
    {'i', "Kernel", "Float", "(arg, exception: true)", "Float", "Converts `arg` to a Float, strictly."},
    {'i', "Kernel", "String", "(arg)", "String", "Converts `arg` to a String."},
    {'i', "Kernel", "Array", "(arg)", "Array", "Converts `arg` to an Array."},
    {'i', "Kernel", "Hash", "(arg)", "Hash", "Converts `arg` to a Hash."},
    {'i', "Kernel", "system", "(*command, exception: false)", "Boolean", "Runs a command in a subshell."},
    {'i', "Kernel", "warn", "(*messages, uplevel: nil, category: nil)", "nil", "Writes the messages to standard error."},
    {'i', "Kernel", "open", "(path, mode = 'r', *args, &block)", "IO", "Opens a file (or a pipe with `|cmd`)."},
    {'i', "Kernel", "freeze", "", "$self", "Prevents further modifications."},
    {'i', "Kernel", "frozen?", "", "Boolean", "Whether the object is frozen."},
    {'i', "Kernel", "tap", "(&block)", "$self", "Yields self to the block, then returns self."},
    {'i', "Kernel", "then", "(&block)", "", "Yields self to the block and returns its result."},
    {'i', "Kernel", "class", "", "Class", "The object's class."},
    {'i', "Kernel", "is_a?", "(klass)", "Boolean", "Whether `klass` is the object's class or one of its ancestors."},
    {'i', "Kernel", "kind_of?", "(klass)", "Boolean", "Same as `is_a?`."},
    {'i', "Kernel", "instance_of?", "(klass)", "Boolean", "Whether the object's class is exactly `klass`."},
    {'i', "Kernel", "respond_to?", "(name, include_all = false)", "Boolean", "Whether the object has public method `name`."},
    {'i', "Kernel", "send", "(name, *args, &block)", "", "Calls method `name` (even a private one)."},
    {'i', "Kernel", "public_send", "(name, *args, &block)", "", "Calls public method `name`."},
    {'i', "Kernel", "method", "(name)", "Method", "The method `name` as an object."},
    {'i', "Kernel", "methods", "", "Array<Symbol>", "The names of the object's public and protected methods."},
    {'i', "Kernel", "nil?", "", "Boolean", "Whether the object is `nil`."},
    {'i', "Kernel", "to_s", "", "String", "A string representation."},
    {'i', "Kernel", "inspect", "", "String", "A human-readable representation, for debugging."},
    {'i', "Kernel", "dup", "", "$self", "A shallow copy."},
    {'i', "Kernel", "clone", "(freeze: nil)", "$self", "A shallow copy, with singleton methods and frozen state."},
    {'i', "Kernel", "hash", "", "Integer", "The hash code."},
    {'i', "Kernel", "object_id", "", "Integer", "The object's unique id."},
    {'i', "Kernel", "instance_variable_get", "(name)", "", "The value of instance variable `name`."},
    {'i', "Kernel", "instance_variable_set", "(name, value)", "", "Sets instance variable `name`."},
    {'i', "Kernel", "instance_variables", "", "Array<Symbol>", "The names of the instance variables."},
    {'i', "Kernel", "extend", "(*modules)", "$self", "Adds the modules' methods to this object."},
    {'i', "Kernel", "define_singleton_method", "(name, &block)", "Symbol", "Defines a singleton method."},
    {'i', "Kernel", "display", "(port = $stdout)", "nil", "Prints the object."},
    {'i', "Kernel", "equal?", "(other)", "Boolean", "Whether both are the same object."},
    {'i', "Kernel", "eql?", "(other)", "Boolean", "Whether both are equal (as hash keys)."},
    {'i', "Kernel", "instance_eval", "(&block)", "", "Runs the block with self set to the object."},

    // Module and Class
    {'i', "Module", "attr_accessor", "(*names)", "Array<Symbol>", "Defines reader and writer methods for the attributes."},
    {'i', "Module", "attr_reader", "(*names)", "Array<Symbol>", "Defines reader methods for the attributes."},
    {'i', "Module", "attr_writer", "(*names)", "Array<Symbol>", "Defines writer methods for the attributes."},
    {'i', "Module", "include", "(*modules)", "$self", "Mixes the modules' instance methods into this one."},
    {'i', "Module", "prepend", "(*modules)", "$self", "Mixes the modules in before this module's own methods."},
    {'i', "Module", "extend", "(*modules)", "$self", "Adds the modules' methods as singleton methods."},
    {'i', "Module", "module_function", "(*names)", "nil", "Makes the methods module functions."},
    {'i', "Module", "private", "(*names)", "nil", "Makes the methods (or the ones that follow) private."},
    {'i', "Module", "protected", "(*names)", "nil", "Makes the methods (or the ones that follow) protected."},
    {'i', "Module", "public", "(*names)", "nil", "Makes the methods (or the ones that follow) public."},
    {'i', "Module", "private_constant", "(*names)", "nil", "Makes the constants private."},
    {'i', "Module", "define_method", "(name, method = nil, &block)", "Symbol", "Defines an instance method."},
    {'i', "Module", "alias_method", "(new_name, old_name)", "Symbol", "Makes `new_name` a copy of method `old_name`."},
    {'i', "Module", "name", "", "String", "The module's name."},
    {'i', "Module", "instance_methods", "(include_inherited = true)", "Array<Symbol>", "The public and protected instance methods."},
    {'i', "Module", "method_defined?", "(name, inherit = true)", "Boolean", "Whether the instance method is defined."},
    {'i', "Module", "const_get", "(name, inherit = true)", "", "The value of constant `name`."},
    {'i', "Module", "constants", "(inherit = true)", "Array<Symbol>", "The names of the constants."},
    {'i', "Module", "ancestors", "", "Array<Module>", "The modules and classes in the lookup chain."},
    {'i', "Module", "class_eval", "(&block)", "", "Runs the block in the module's context."},
    {'i', "Module", "module_eval", "(&block)", "", "Runs the block in the module's context."},
    {'i', "Class", "new", "(*args, &block)", "$instance", "Allocates an instance and calls its `initialize`."},
    {'i', "Class", "allocate", "", "$instance", "Allocates an instance without calling `initialize`."},
    {'i', "Class", "superclass", "", "Class", "The superclass."},

    // Comparable
    {'i', "Comparable", "between?", "(min, max)", "Boolean", "Whether the value is in [min, max]."},
    {'i', "Comparable", "clamp", "(min, max)", "$self", "The value limited to [min, max]."},

    // Enumerable (also Array, Hash, Range)
    {'i', "Enumerable", "map", "(&block)", "Array", "The block's results for each element."},
    {'i', "Enumerable", "collect", "(&block)", "Array", "Same as `map`."},
    {'i', "Enumerable", "flat_map", "(&block)", "Array", "`map`, then flattened one level."},
    {'i', "Enumerable", "select", "(&block)", "Array<$elem>", "The elements for which the block is truthy."},
    {'i', "Enumerable", "filter", "(&block)", "Array<$elem>", "Same as `select`."},
    {'i', "Enumerable", "filter_map", "(&block)", "Array", "The truthy results of the block."},
    {'i', "Enumerable", "reject", "(&block)", "Array<$elem>", "The elements for which the block is falsy."},
    {'i', "Enumerable", "find", "(ifnone = nil, &block)", "$elem", "The first element for which the block is truthy."},
    {'i', "Enumerable", "detect", "(ifnone = nil, &block)", "$elem", "Same as `find`."},
    {'i', "Enumerable", "find_index", "(value = nil, &block)", "Integer", "The index of the first match."},
    {'i', "Enumerable", "reduce", "(initial = nil, sym = nil, &block)", "", "Combines the elements with the block or operator."},
    {'i', "Enumerable", "inject", "(initial = nil, sym = nil, &block)", "", "Same as `reduce`."},
    {'i', "Enumerable", "sum", "(init = 0, &block)", "", "The sum of the elements (or of the block's results)."},
    {'i', "Enumerable", "count", "(item = nil, &block)", "Integer", "The number of elements (matching)."},
    {'i', "Enumerable", "min", "(n = nil, &block)", "$elem", "The smallest element."},
    {'i', "Enumerable", "max", "(n = nil, &block)", "$elem", "The largest element."},
    {'i', "Enumerable", "min_by", "(n = nil, &block)", "$elem", "The element with the smallest block result."},
    {'i', "Enumerable", "max_by", "(n = nil, &block)", "$elem", "The element with the largest block result."},
    {'i', "Enumerable", "minmax", "", "Array<$elem>", "The smallest and the largest element."},
    {'i', "Enumerable", "sort", "(&block)", "Array<$elem>", "The elements, sorted."},
    {'i', "Enumerable", "sort_by", "(&block)", "Array<$elem>", "The elements, sorted by the block's result."},
    {'i', "Enumerable", "group_by", "(&block)", "Hash", "The elements grouped by the block's result."},
    {'i', "Enumerable", "partition", "(&block)", "Array", "Two arrays: the elements for which the block is truthy, and the rest."},
    {'i', "Enumerable", "each_with_index", "(*args, &block)", "$self", "Yields each element and its index."},
    {'i', "Enumerable", "each_with_object", "(object, &block)", "", "Yields each element and `object`; returns `object`."},
    {'i', "Enumerable", "each_slice", "(n, &block)", "", "Yields consecutive slices of `n` elements."},
    {'i', "Enumerable", "each_cons", "(n, &block)", "", "Yields each run of `n` consecutive elements."},
    {'i', "Enumerable", "include?", "(object)", "Boolean", "Whether an element `==` object."},
    {'i', "Enumerable", "member?", "(object)", "Boolean", "Same as `include?`."},
    {'i', "Enumerable", "any?", "(pattern = nil, &block)", "Boolean", "Whether any element matches."},
    {'i', "Enumerable", "all?", "(pattern = nil, &block)", "Boolean", "Whether every element matches."},
    {'i', "Enumerable", "none?", "(pattern = nil, &block)", "Boolean", "Whether no element matches."},
    {'i', "Enumerable", "first", "(n = nil)", "$elem", "The first element (or the first `n`)."},
    {'i', "Enumerable", "take", "(n)", "Array<$elem>", "The first `n` elements."},
    {'i', "Enumerable", "drop", "(n)", "Array<$elem>", "All but the first `n` elements."},
    {'i', "Enumerable", "take_while", "(&block)", "Array<$elem>", "The leading elements for which the block is truthy."},
    {'i', "Enumerable", "zip", "(*others)", "Array", "The elements paired with the others' elements."},
    {'i', "Enumerable", "tally", "", "Hash", "Counts of each distinct element."},
    {'i', "Enumerable", "uniq", "(&block)", "Array<$elem>", "The distinct elements."},
    {'i', "Enumerable", "to_a", "", "Array<$elem>", "The elements as an array."},
    {'i', "Enumerable", "to_h", "(&block)", "Hash", "The pairs as a hash."},
    {'i', "Enumerable", "to_set", "", "Set", "The elements as a set."},
    {'i', "Enumerable", "each_entry", "(&block)", "$self", "Yields each element."},
    {'i', "Enumerable", "lazy", "", "Enumerator::Lazy", "A lazy enumerator."},
    {'i', "Enumerable", "chunk_while", "(&block)", "Enumerator", "Splits where the block is falsy between neighbours."},

    // Array
    {'s', "Array", "new", "(size = 0, default = nil, &block)", "$instance", "A new array."},
    {'i', "Array", "each", "(&block)", "$self", "Yields each element."},
    {'i', "Array", "each_index", "(&block)", "$self", "Yields each index."},
    {'i', "Array", "map!", "(&block)", "$self", "Replaces each element with the block's result."},
    {'i', "Array", "select!", "(&block)", "$self", "Keeps the elements for which the block is truthy."},
    {'i', "Array", "reject!", "(&block)", "$self", "Removes the elements for which the block is truthy."},
    {'i', "Array", "length", "", "Integer", "The number of elements."},
    {'i', "Array", "size", "", "Integer", "The number of elements."},
    {'i', "Array", "empty?", "", "Boolean", "Whether the array has no elements."},
    {'i', "Array", "last", "(n = nil)", "$elem", "The last element (or the last `n`)."},
    {'i', "Array", "push", "(*objects)", "$self", "Appends the objects."},
    {'i', "Array", "append", "(*objects)", "$self", "Appends the objects."},
    {'i', "Array", "<<", "(object)", "$self", "Appends `object`."},
    {'i', "Array", "pop", "(n = nil)", "$elem", "Removes and returns the last element."},
    {'i', "Array", "shift", "(n = nil)", "$elem", "Removes and returns the first element."},
    {'i', "Array", "unshift", "(*objects)", "$self", "Prepends the objects."},
    {'i', "Array", "insert", "(index, *objects)", "$self", "Inserts the objects before `index`."},
    {'i', "Array", "concat", "(*arrays)", "$self", "Appends the arrays' elements."},
    {'i', "Array", "delete", "(object)", "$elem", "Removes every element `==` object."},
    {'i', "Array", "delete_at", "(index)", "$elem", "Removes the element at `index`."},
    {'i', "Array", "delete_if", "(&block)", "$self", "Removes the elements for which the block is truthy."},
    {'i', "Array", "index", "(object = nil, &block)", "Integer", "The index of the first match."},
    {'i', "Array", "join", "(separator = $,)", "String", "The elements as strings, joined."},
    {'i', "Array", "flatten", "(depth = nil)", "Array", "Nested arrays merged into one."},
    {'i', "Array", "compact", "", "Array<$elem>", "The elements without nils."},
    {'i', "Array", "reverse", "", "Array<$elem>", "The elements in reverse order."},
    {'i', "Array", "rotate", "(count = 1)", "Array<$elem>", "The elements rotated by `count`."},
    {'i', "Array", "sample", "(n = nil, random: Random)", "$elem", "A random element."},
    {'i', "Array", "shuffle", "(random: Random)", "Array<$elem>", "The elements in random order."},
    {'i', "Array", "slice", "(start, length = nil)", "Array<$elem>", "A subarray (or one element)."},
    {'i', "Array", "dig", "(index, *identifiers)", "", "The nested value at the indexes."},
    {'i', "Array", "fill", "(value = nil, start = nil, length = nil, &block)", "$self", "Fills the array."},
    {'i', "Array", "product", "(*others)", "Array", "The Cartesian product."},
    {'i', "Array", "transpose", "", "Array", "Rows and columns swapped."},
    {'i', "Array", "combination", "(n, &block)", "Enumerator", "The `n`-element combinations."},
    {'i', "Array", "permutation", "(n = nil, &block)", "Enumerator", "The `n`-element permutations."},
    {'i', "Array", "pack", "(template, buffer: nil)", "String", "The elements packed into a binary string."},
    {'i', "Array", "sort!", "(&block)", "$self", "Sorts in place."},
    {'i', "Array", "uniq!", "(&block)", "$self", "Removes duplicates in place."},
    {'i', "Array", "clear", "", "$self", "Removes every element."},
    {'i', "Array", "fetch", "(index, default = nil, &block)", "$elem", "The element at `index`; raises IndexError when out of range."},
    {'i', "Array", "values_at", "(*indexes)", "Array<$elem>", "The elements at the indexes."},
    {'i', "Array", "intersection", "(*others)", "Array<$elem>", "The elements in every array."},
    {'i', "Array", "union", "(*others)", "Array", "The distinct elements of all arrays."},
    {'i', "Array", "difference", "(*others)", "Array<$elem>", "The elements not in the others."},

    // Hash
    {'s', "Hash", "new", "(default = nil, capacity: 0, &block)", "$instance", "A new hash."},
    {'i', "Hash", "each", "(&block)", "$self", "Yields each key and value."},
    {'i', "Hash", "each_pair", "(&block)", "$self", "Yields each key and value."},
    {'i', "Hash", "each_key", "(&block)", "$self", "Yields each key."},
    {'i', "Hash", "each_value", "(&block)", "$self", "Yields each value."},
    {'i', "Hash", "keys", "", "Array", "The keys."},
    {'i', "Hash", "values", "", "Array", "The values."},
    {'i', "Hash", "fetch", "(key, default = nil, &block)", "", "The value for `key`; raises KeyError when missing."},
    {'i', "Hash", "key?", "(key)", "Boolean", "Whether `key` is present."},
    {'i', "Hash", "has_key?", "(key)", "Boolean", "Whether `key` is present."},
    {'i', "Hash", "value?", "(value)", "Boolean", "Whether some key maps to `value`."},
    {'i', "Hash", "dig", "(key, *identifiers)", "", "The nested value at the keys."},
    {'i', "Hash", "merge", "(*others, &block)", "Hash", "A new hash with the others' pairs added."},
    {'i', "Hash", "merge!", "(*others, &block)", "$self", "Adds the others' pairs."},
    {'i', "Hash", "update", "(*others, &block)", "$self", "Adds the others' pairs."},
    {'i', "Hash", "delete", "(key, &block)", "", "Removes `key`; returns its value."},
    {'i', "Hash", "store", "(key, value)", "", "Sets the value for `key`."},
    {'i', "Hash", "size", "", "Integer", "The number of pairs."},
    {'i', "Hash", "length", "", "Integer", "The number of pairs."},
    {'i', "Hash", "empty?", "", "Boolean", "Whether the hash has no pairs."},
    {'i', "Hash", "transform_values", "(&block)", "Hash", "A new hash with each value replaced by the block's result."},
    {'i', "Hash", "transform_keys", "(mapping = nil, &block)", "Hash", "A new hash with each key replaced."},
    {'i', "Hash", "select", "(&block)", "Hash", "The pairs for which the block is truthy."},
    {'i', "Hash", "filter", "(&block)", "Hash", "The pairs for which the block is truthy."},
    {'i', "Hash", "reject", "(&block)", "Hash", "The pairs for which the block is falsy."},
    {'i', "Hash", "slice", "(*keys)", "Hash", "The pairs for the keys."},
    {'i', "Hash", "except", "(*keys)", "Hash", "The pairs without the keys."},
    {'i', "Hash", "values_at", "(*keys)", "Array", "The values for the keys."},
    {'i', "Hash", "key", "(value)", "", "The first key that maps to `value`."},
    {'i', "Hash", "invert", "", "Hash", "Keys and values swapped."},
    {'i', "Hash", "default", "(key = nil)", "", "The default value."},
    {'i', "Hash", "to_a", "", "Array", "The pairs as `[key, value]` arrays."},
    {'i', "Hash", "sort_by", "(&block)", "Array", "The pairs sorted by the block's result."},
    {'i', "Hash", "compact", "", "Hash", "The pairs without nil values."},
    {'i', "Hash", "clear", "", "$self", "Removes every pair."},

    // String
    {'s', "String", "new", "(string = '', encoding: nil, capacity: nil)", "$instance", "A new string."},
    {'i', "String", "length", "", "Integer", "The number of characters."},
    {'i', "String", "size", "", "Integer", "The number of characters."},
    {'i', "String", "bytesize", "", "Integer", "The number of bytes."},
    {'i', "String", "empty?", "", "Boolean", "Whether the string is empty."},
    {'i', "String", "upcase", "(*options)", "String", "Uppercased."},
    {'i', "String", "downcase", "(*options)", "String", "Lowercased."},
    {'i', "String", "capitalize", "(*options)", "String", "The first character uppercased, the rest lowercased."},
    {'i', "String", "swapcase", "(*options)", "String", "Cases swapped."},
    {'i', "String", "strip", "", "String", "Leading and trailing whitespace removed."},
    {'i', "String", "lstrip", "", "String", "Leading whitespace removed."},
    {'i', "String", "rstrip", "", "String", "Trailing whitespace removed."},
    {'i', "String", "chomp", "(separator = $/)", "String", "The trailing newline (or separator) removed."},
    {'i', "String", "chop", "", "String", "The last character removed."},
    {'i', "String", "chars", "", "Array<String>", "The characters."},
    {'i', "String", "bytes", "", "Array<Integer>", "The bytes."},
    {'i', "String", "lines", "(separator = $/, chomp: false)", "Array<String>", "The lines."},
    {'i', "String", "split", "(pattern = nil, limit = 0)", "Array<String>", "The substrings between matches of `pattern`."},
    {'i', "String", "sub", "(pattern, replacement = nil, &block)", "String", "The first match of `pattern` replaced."},
    {'i', "String", "gsub", "(pattern, replacement = nil, &block)", "String", "Every match of `pattern` replaced."},
    {'i', "String", "sub!", "(pattern, replacement = nil, &block)", "$self", "Replaces the first match in place."},
    {'i', "String", "gsub!", "(pattern, replacement = nil, &block)", "$self", "Replaces every match in place."},
    {'i', "String", "tr", "(from, to)", "String", "Characters translated."},
    {'i', "String", "delete", "(*selectors)", "String", "The selected characters removed."},
    {'i', "String", "squeeze", "(*selectors)", "String", "Runs of a character reduced to one."},
    {'i', "String", "count", "(*selectors)", "Integer", "The number of selected characters."},
    {'i', "String", "include?", "(other)", "Boolean", "Whether `other` is a substring."},
    {'i', "String", "start_with?", "(*prefixes)", "Boolean", "Whether the string starts with one of the prefixes."},
    {'i', "String", "end_with?", "(*suffixes)", "Boolean", "Whether the string ends with one of the suffixes."},
    {'i', "String", "index", "(pattern, offset = 0)", "Integer", "The index of the first match."},
    {'i', "String", "rindex", "(pattern, offset = nil)", "Integer", "The index of the last match."},
    {'i', "String", "slice", "(start, length = nil)", "String", "A substring."},
    {'i', "String", "match", "(pattern, offset = 0)", "MatchData", "The first match of `pattern`, or nil."},
    {'i', "String", "match?", "(pattern, offset = 0)", "Boolean", "Whether `pattern` matches."},
    {'i', "String", "scan", "(pattern, &block)", "Array", "Every match of `pattern`."},
    {'i', "String", "to_i", "(base = 10)", "Integer", "The leading integer."},
    {'i', "String", "to_f", "", "Float", "The leading float."},
    {'i', "String", "to_sym", "", "Symbol", "The string as a symbol."},
    {'i', "String", "to_s", "", "String", "The string itself."},
    {'i', "String", "reverse", "", "String", "The characters in reverse order."},
    {'i', "String", "center", "(width, pad = ' ')", "String", "Centered in `width` columns."},
    {'i', "String", "ljust", "(width, pad = ' ')", "String", "Left-justified in `width` columns."},
    {'i', "String", "rjust", "(width, pad = ' ')", "String", "Right-justified in `width` columns."},
    {'i', "String", "each_char", "(&block)", "$self", "Yields each character."},
    {'i', "String", "each_line", "(separator = $/, chomp: false, &block)", "$self", "Yields each line."},
    {'i', "String", "encoding", "", "Encoding", "The encoding."},
    {'i', "String", "force_encoding", "(encoding)", "$self", "Changes the encoding (not the bytes)."},
    {'i', "String", "encode", "(encoding, **options)", "String", "Transcoded to `encoding`."},
    {'i', "String", "unpack", "(template, offset: 0)", "Array", "Binary data unpacked."},
    {'i', "String", "replace", "(other)", "$self", "Replaces the contents."},
    {'i', "String", "freeze", "", "$self", "Freezes the string."},
    {'i', "String", "+", "(other)", "String", "Concatenation."},
    {'i', "String", "*", "(n)", "String", "Repeated `n` times."},
    {'i', "String", "%", "(args)", "String", "Formatted with `format`."},
    {'i', "String", "<<", "(object)", "$self", "Appends `object`."},
    {'i', "String", "=~", "(pattern)", "Integer", "The index of the first match, or nil."},

    // Symbol
    {'i', "Symbol", "to_s", "", "String", "The symbol's name."},
    {'i', "Symbol", "to_proc", "", "Proc", "A proc that calls the method named by the symbol."},
    {'i', "Symbol", "length", "", "Integer", "The length of the name."},

    // Numbers
    {'i', "Numeric", "abs", "", "$self", "The absolute value."},
    {'i', "Numeric", "zero?", "", "Boolean", "Whether the value is zero."},
    {'i', "Numeric", "positive?", "", "Boolean", "Whether the value is greater than zero."},
    {'i', "Numeric", "negative?", "", "Boolean", "Whether the value is less than zero."},
    {'i', "Numeric", "round", "(ndigits = 0, half: :up)", "$self", "Rounded to `ndigits` decimal digits."},
    {'i', "Numeric", "floor", "(ndigits = 0)", "Integer", "Rounded down."},
    {'i', "Numeric", "ceil", "(ndigits = 0)", "Integer", "Rounded up."},
    {'i', "Numeric", "truncate", "(ndigits = 0)", "Integer", "Rounded toward zero."},
    {'i', "Numeric", "divmod", "(other)", "Array", "Quotient and modulus."},
    {'i', "Numeric", "step", "(limit = nil, step = 1, &block)", "", "Yields values from self to `limit` by `step`."},
    {'i', "Numeric", "to_i", "", "Integer", "Truncated to an Integer."},
    {'i', "Numeric", "to_f", "", "Float", "As a Float."},
    {'i', "Numeric", "to_s", "", "String", "As a String."},
    {'i', "Numeric", "integer?", "", "Boolean", "Whether the value is an Integer."},
    {'i', "Integer", "times", "(&block)", "$self", "Yields 0 through self - 1."},
    {'i', "Integer", "upto", "(limit, &block)", "$self", "Yields self through `limit`."},
    {'i', "Integer", "downto", "(limit, &block)", "$self", "Yields self down to `limit`."},
    {'i', "Integer", "even?", "", "Boolean", "Whether the integer is even."},
    {'i', "Integer", "odd?", "", "Boolean", "Whether the integer is odd."},
    {'i', "Integer", "succ", "", "Integer", "self + 1."},
    {'i', "Integer", "pred", "", "Integer", "self - 1."},
    {'i', "Integer", "pow", "(exponent, modulus = nil)", "Integer", "self raised to `exponent` (modulo `modulus`)."},
    {'i', "Integer", "gcd", "(other)", "Integer", "The greatest common divisor."},
    {'i', "Integer", "lcm", "(other)", "Integer", "The least common multiple."},
    {'i', "Integer", "digits", "(base = 10)", "Array<Integer>", "The digits, least significant first."},
    {'i', "Integer", "chr", "(encoding = nil)", "String", "The character with this code."},
    {'i', "Integer", "bit_length", "", "Integer", "The number of bits needed."},
    {'i', "Float", "nan?", "", "Boolean", "Whether the value is NaN."},
    {'i', "Float", "infinite?", "", "Integer", "1 or -1 when infinite, else nil."},
    {'i', "Float", "finite?", "", "Boolean", "Whether the value is finite."},

    // Range
    {'i', "Range", "each", "(&block)", "$self", "Yields each element."},
    {'i', "Range", "step", "(n = 1, &block)", "", "Yields every `n`th element."},
    {'i', "Range", "cover?", "(object)", "Boolean", "Whether `object` is between the ends."},
    {'i', "Range", "begin", "", "", "The first end."},
    {'i', "Range", "end", "", "", "The last end."},
    {'i', "Range", "size", "", "Integer", "The number of elements."},
    {'i', "Range", "last", "(n = nil)", "", "The last element (or the last `n`)."},

    // Proc
    {'i', "Proc", "call", "(*args, &block)", "", "Calls the proc."},
    {'i', "Proc", "arity", "", "Integer", "The number of mandatory arguments."},
    {'i', "Proc", "curry", "(arity = nil)", "Proc", "A curried proc."},
    {'i', "Proc", "lambda?", "", "Boolean", "Whether the proc is a lambda."},

    // NilClass
    {'i', "NilClass", "to_a", "", "Array", "An empty array."},
    {'i', "NilClass", "to_s", "", "String", "An empty string."},
    {'i', "NilClass", "to_i", "", "Integer", "Zero."},

    // Regexp / MatchData
    {'i', "Regexp", "match", "(string, pos = 0)", "MatchData", "The first match in `string`, or nil."},
    {'i', "Regexp", "match?", "(string, pos = 0)", "Boolean", "Whether the pattern matches."},
    {'i', "Regexp", "source", "", "String", "The pattern's source."},
    {'i', "MatchData", "captures", "", "Array<String>", "The captured groups."},
    {'i', "MatchData", "named_captures", "", "Hash", "The named groups."},
    {'i', "MatchData", "pre_match", "", "String", "The text before the match."},
    {'i', "MatchData", "post_match", "", "String", "The text after the match."},

    // Exception
    {'s', "Exception", "new", "(message = nil)", "$instance", "A new exception."},
    {'i', "Exception", "message", "", "String", "The message."},
    {'i', "Exception", "backtrace", "", "Array<String>", "The backtrace."},
    {'i', "Exception", "full_message", "(highlight: false, order: :top)", "String", "The message with its backtrace."},
    {'i', "Exception", "cause", "", "Exception", "The exception being handled when this one was raised."},

    // Time
    {'s', "Time", "now", "", "Time", "The current time."},
    {'s', "Time", "at", "(time, subsec = 0, unit = :microsecond, in: nil)", "Time", "The time `time` seconds after the epoch."},
    {'i', "Time", "to_f", "", "Float", "Seconds since the epoch."},
    {'i', "Time", "to_i", "", "Integer", "Whole seconds since the epoch."},
    {'i', "Time", "strftime", "(format)", "String", "Formatted with `format` directives."},
    {'i', "Time", "year", "", "Integer", "The year."},

    // IO / File / Dir
    {'s', "File", "read", "(path, length = nil, offset = nil, **opts)", "String", "The file's contents."},
    {'s', "File", "write", "(path, data, offset = nil, **opts)", "Integer", "Writes `data` to the file."},
    {'s', "File", "readlines", "(path, sep = $/, chomp: false)", "Array<String>", "The file's lines."},
    {'s', "File", "foreach", "(path, sep = $/, chomp: false, &block)", "nil", "Yields each line of the file."},
    {'s', "File", "open", "(path, mode = 'r', **opts, &block)", "File", "Opens a file (closed after the block)."},
    {'s', "File", "exist?", "(path)", "Boolean", "Whether the path exists."},
    {'s', "File", "file?", "(path)", "Boolean", "Whether the path is a regular file."},
    {'s', "File", "directory?", "(path)", "Boolean", "Whether the path is a directory."},
    {'s', "File", "basename", "(path, suffix = '')", "String", "The last component of the path."},
    {'s', "File", "dirname", "(path, level = 1)", "String", "The directory part of the path."},
    {'s', "File", "extname", "(path)", "String", "The extension."},
    {'s', "File", "join", "(*parts)", "String", "The parts joined with `/`."},
    {'s', "File", "expand_path", "(path, dir = nil)", "String", "The absolute path."},
    {'s', "File", "size", "(path)", "Integer", "The file's size in bytes."},
    {'s', "Dir", "glob", "(pattern, flags = 0, base: nil, sort: true)", "Array<String>", "The paths matching `pattern`."},
    {'s', "Dir", "pwd", "", "String", "The current directory."},
    {'s', "Dir", "children", "(path)", "Array<String>", "The entries of the directory (without `.` and `..`)."},
    {'s', "Dir", "mkdir", "(path, mode = 0o777)", "Integer", "Creates a directory."},
    {'i', "IO", "puts", "(*objects)", "nil", "Writes each object and a newline."},
    {'i', "IO", "print", "(*objects)", "nil", "Writes the objects."},
    {'i', "IO", "write", "(*objects)", "Integer", "Writes the objects; returns the bytes written."},
    {'i', "IO", "read", "(length = nil, buffer = nil)", "String", "Reads (the rest of) the stream."},
    {'i', "IO", "gets", "(sep = $/, chomp: false)", "String", "Reads a line."},
    {'i', "IO", "each_line", "(sep = $/, chomp: false, &block)", "$self", "Yields each line."},
    {'i', "IO", "close", "", "nil", "Closes the stream."},
    {'i', "IO", "flush", "", "$self", "Flushes buffered output."},

    // Math
    {'s', "Math", "sqrt", "(x)", "Float", "The square root of `x`."},
    {'s', "Math", "cbrt", "(x)", "Float", "The cube root of `x`."},
    {'s', "Math", "sin", "(x)", "Float", "The sine of `x` (radians)."},
    {'s', "Math", "cos", "(x)", "Float", "The cosine of `x` (radians)."},
    {'s', "Math", "tan", "(x)", "Float", "The tangent of `x` (radians)."},
    {'s', "Math", "atan2", "(y, x)", "Float", "The arc tangent of `y/x`, in the right quadrant."},
    {'s', "Math", "exp", "(x)", "Float", "e raised to `x`."},
    {'s', "Math", "log", "(x, base = Math::E)", "Float", "The logarithm of `x`."},
    {'s', "Math", "log2", "(x)", "Float", "The base-2 logarithm of `x`."},
    {'s', "Math", "log10", "(x)", "Float", "The base-10 logarithm of `x`."},
    {'s', "Math", "hypot", "(x, y)", "Float", "sqrt(x² + y²)."},
};

std::shared_ptr<FileIndex> Build() {
    auto f = std::make_shared<FileIndex>();
    f->path = "<builtin>";
    for (const Entry &e : kEntries) {
        auto d = std::make_unique<Def>();
        d->builtin = true;
        d->path = "<builtin>";
        d->name = e.name;
        d->owner = e.owner;
        std::vector<std::string> doc_lines{e.doc};
        d->doc = ParseYard(doc_lines);
        switch (e.kind) {
        case 'c':
        case 'm': {
            d->kind = e.kind == 'c' ? DefKind::Class : DefKind::Module;
            d->fqn = *e.owner ? std::string(e.owner) + "::" + e.name : std::string(e.name);
            d->superclass = e.params;
            const std::string incs = e.ret;
            size_t start = 0;
            while (start < incs.size()) {
                size_t comma = incs.find(',', start);
                if (comma == std::string::npos) comma = incs.size();
                std::string inc = incs.substr(start, comma - start);
                while (!inc.empty() && inc[0] == ' ') inc.erase(0, 1);
                if (!inc.empty()) d->includes.push_back(inc);
                start = comma + 1;
            }
            break;
        }
        case 'k':
            d->kind = DefKind::Constant;
            d->fqn = *e.owner ? std::string(e.owner) + "::" + e.name : std::string(e.name);
            d->return_type = e.ret;
            break;
        default:
            d->kind = DefKind::Method;
            d->singleton = e.kind == 's';
            d->params = ParseParamList(e.params);
            d->return_type = e.ret;
            break;
        }
        f->defs.push_back(std::move(d));
    }
    return f;
}

}  // namespace

std::shared_ptr<const FileIndex> Builtins() {
    static const std::shared_ptr<const FileIndex> f = Build();
    return f;
}

const std::vector<std::string> &Keywords() {
    static const std::vector<std::string> k = {
        "BEGIN", "END", "alias", "and", "begin", "break", "case", "class", "def", "defined?", "do", "else",
        "elsif", "end", "ensure", "false", "for", "if", "in", "module", "next", "nil", "not", "or", "redo",
        "rescue", "retry", "return", "self", "super", "then", "true", "undef", "unless", "until", "when",
        "while", "yield", "__FILE__", "__LINE__", "__dir__", "__method__"};
    return k;
}

}  // namespace rblsp
