# LX Language Reference
## File extension: `.lx`  |  Interpreter: `lx <file.lx>`

---

## 1 · Comments
```
~~ this is a comment
```

---

## 2 · Print
| Keyword          | Behaviour                    |
|------------------|------------------------------|
| `with_text:`     | Print expression + newline   |
| `with_text_raw:` | Print expression, no newline |
| `scream:`        | Print to **stderr** (errors) |

```
with_text: "Hello, World!"
with_text_raw: "Value = "
with_text: 42
```

---

## 3 · Data Types & Declaration
Syntax: `d_type: TYPE  VARNAME  ::  EXPRESSION`

The `::` symbol is the assignment / equality operator.

| Type     | Description              |
|----------|--------------------------|
| `short`  | Small integer            |
| `long`   | Large integer            |
| `int`    | Integer (alias)          |
| `float`  | Floating-point           |
| `double` | Double-precision float   |
| `string` | Text                     |
| `char`   | Single character         |
| `bool`   | `VERUM` or `FALSUM`      |

```
d_type: string  name    :: "Ada"
d_type: long    age     :: 30
d_type: double  pi      :: 3.14159
d_type: bool    active  :: VERUM
d_type: char    grade   :: "A"
```

---

## 4 · Reassignment
```
reassign: age :: age + 1
```
The variable must have been declared with `d_type:` first.

---

## 5 · Operators
### Arithmetic
`+`  `-`  `*`  `/`  `%`
String concatenation uses `+`.

### Comparison
| LX Keyword | Meaning          |
|------------|------------------|
| `IS`       | equal            |
| `IS_NOT`   | not equal        |
| `GT`       | greater than     |
| `LT`       | less than        |
| `GT_EQ`    | greater or equal |
| `LT_EQ`    | less or equal    |

### Logical
`AND_W`  `OR_W`  `NOT_W`

---

## 6 · Conditionals  (`maybe` / `perhaps_not` / `end_maybe`)
```
maybe: age GT 18
    with_text: "adult"
perhaps_not:
    with_text: "minor"
end_maybe:
```
`perhaps_not:` block is optional. Blocks nest freely.

---

## 7 · Loops

### `loop_from` (counted, like FOR)
```
loop_from: i FROM 1 TO 10
    with_text: i
end_loop:

~~ with a step
loop_from: j FROM 0 TO 100 STEP 10
    with_text: j
end_loop:
```

### `loop_while` (condition-based)
```
d_type: long n :: 0
loop_while: n LT 5
    with_text: n
    reassign: n :: n + 1
end_loop:
```

---

## 8 · Functions

### Define
```
func_begin: add(a, b)
    give_back: a + b
func_end:
```
Function definitions can appear **anywhere** in the file; they are hoisted during the pre-pass.

### Call (statement)
```
call: greet("World")
```

### Call (expression / return value)
```
d_type: long  result :: add(3, 4)
```

### Return value
```
give_back: EXPRESSION
```

---

## 9 · Jump Instructions  (assembly-style)
### Declare a label
```
label_mark: MY_LABEL
```

### Unconditional jump
```
jump: MY_LABEL
```

### Conditional jump
```
jump_if: x GT 10 -> BIG_NUMBER
```

---

## 10 · Imports / Modules
```
with_get <mathlib.lx>
with_get <utils>          ~~ .lx extension is optional
```
All functions and labels from the imported file are merged before execution.

---

## 11 · Special Values
| Literal   | Meaning      |
|-----------|--------------|
| `VERUM`   | true         |
| `FALSUM`  | false        |
| `VOIDUM`  | empty string |

---

## 12 · Built-in Functions
| Call          | Returns                  |
|---------------|--------------------------|
| `len(x)`      | Length of string         |
| `str(x)`      | Convert to string        |
| `num(x)`      | Convert to number        |
| `sqrt(x)`     | Square root              |
| `abs(x)`      | Absolute value           |
| `input(prompt)` | Read line from stdin   |

---

## 13 · Halt
```
halt_now:
```
Immediately stops execution.

---

## 14 · Full Example
```
with_get <mathlib.lx>

d_type: string  msg  :: "LX is alive"
with_text: msg

loop_from: i FROM 1 TO 5
    d_type: long  f :: factorial(i)
    with_text_raw: i
    with_text_raw: "! = "
    with_text: f
end_loop:

maybe: VERUM IS VERUM
    with_text: "Always runs"
end_maybe:

jump: END_OF_PROG
with_text: "Never reached"
label_mark: END_OF_PROG

halt_now:
```
