# MNT System Controller

## SC Syntax and Types

Every command and response is a list of words, delimited by single spaces and enclosed in parentheses. Commands and responses have the same shape. Examples:

	(hello)
	(242)
	(set-usb 0 1)

Unlike in Lisp, lists are not linked lists but fixed-size arrays with a maximum number of words. All lists are preallocated in SC memory--it is not possible to dynamically create new lists, to prevent any out-of-memory conditions. It is possible to have empty, reserved space in predefined lists for user data.

### U64

Words are 64-bit unsigned integers by default. Even the words `hello` and `set-usb` are such `u64`s. 

The following are three equivalent ways of expressing `u64` words:

- An alphanumeric word has 1-8 characters, must start with a letter from the set `[a-zA-Z]`, followed by characters from the set `[a-zA-Z0-9\-]` (i.e. lower and uppercase ASCII letters, digits and the dash `-`). A word shorter than 8 characters is 0 (NUL) padded on the right side internally. This means alphanumeric words are a subset of `u64` numbers and can be compared and stored efficiently in code. The alphanumeric style is meant to name somewhat human-readable functions, commands and variables without having to deal with string processing in a memory and CPU constrained environment--internally, they're just 64-bit numbers.

- A decimal u64 is an unsigned 64-bit integer that starts with and consists only of digits (`[0-9]`), for example `1234` or `294308572021380970`.

- A hexadecimal u64 is an unsigned 64-bit integer that starts with a hash (`#`) followed by up to 8 hexadecimal digits from the set `[0-9][a-f]`, for example `#ff008002`.

- An `i64` is a twos-complement signed 64-bit integer stored internally in an `u64`. Any number starting with a `-` (dash) is interpreted as an `i64`, as are integers returned from functions with `i64` type or integers passed to a function argument with `i64` type. This is mainly useful for expressing negative voltages or currents. (NOTE: overflows/wraps can happen when passing a large unsigned number as an i64--in the future, this should result in an error).

### Strings (Text)

- Strings start and end with an ASCII quote character (`"`) and can have otherwise arbitrary content (NOTE: this might change in the future)
- Strings are fixed-size and limited to 128 characters. NUL termination can be used for determining if string display should end before 128 characters.
- The total space for strings is fixed and preallocated.
- A list can only include max. 1 string, but in any location in the list.
- Strings are meant for user-friendly descriptive text or for storing small notes/objects/fragments of data.

### Functions

A word can be declared with the type `func`, which means that it is a callable function. A function internally is an alphanumeric word (the "name") that maps to a structure which contains:

- a C function pointer in the SC memory
- an arity (number of arguments)
- an array containing the type of each argument
- a return type

Functions can be called by sending a list that starts with the function name followed by its arguments, if any. Examples:

   (set-usb 0 1)
   (vdm #ff008002 0)
   (pdreset)
   (set-gpio 5 0)

### Void

Void is a special type that is useful as a return type for functions and means "nothing". If a function has a `void` type, it always automatically evaluates to the word `void` after being called.

### Namespaces

There is currently only one global namespace. Words can be somewhat grouped using prefixes with `-` (dash) characters, for example `disp-foo` and `disp-bar` could group display-related words. A mini note taking app could use "file" names like `note-000` till `note-999` etc. These limits are meant to prevent bloating of the system complexity.

### Error codes

TODO

## Command Discovery by Remote UI (Keyboard/OLED)

### Get number of known words

Command:

    (words)

Example response:

	(23)

### Get meaning of word 0

	(word 0)

Example responses:

    (pack-crg "Battery pack charge %" u64 99)
                                      |   |
                                      |   `-- values of u64s are included for convenience
                                      `------ type of this word 
    (set-usb "Set USB port mode: <port> <mode>" func void u64 u64)
                                                  |    |    `------- list of argument types
                                                  |    `------------ return type
                                                  `----------------- this word is a function

### Get number of known commands

As the full word list is bound to be cluttered, commands are a subset of words that are intended to be offered as useful menu items by a remote UI.

	(commands)

Returns number of known commands. Commands are predefined function calls (a bit like scripts or bookmarklets).

### Get command description

    (command 12)

Returns the description of command 12, for example:

    ("USB 2: Sysctl Mode" set-usb 1 0)
     |                    |
     `-- menu item string `-- cmd+args sent to SC

Another example:

    (command 23)

Response:

    ("Reset USB-C PD" pdreset)

These commands and their numbers are just examples and do not exist exactly like that. To discover the actual commands, code in the UI (keyboard) uses `(commands)` to get the total count, and then calls `(command n)` repeatedly, where `n` is a number from `0` to `count-1`, or more efficiently just shows a subset that fits on the (scrolled) display.

## Useful commands for drivers

NOTE: This is a work-in-progress and can change without notice until the next stable release.

### Get main board version

### Get SC firmware version

### Toggle power rails

### Toggle GPIOs

### Set display backlight brightness

### Enter/exit sleep/resume

# TODO

- Lets skip u64 verbosity in responses, words starting with a digit are assumed to be u64.
- Document error codes
- Should there be named constants for things like GPIOs rather than just numbers?
- `settings` list for settings widgets with parameter value ranges, for example for brightness/rgb sliders or multiple choice
