# TCC Test Suite Report for RCC
Generated: April 2026

## Summary
 - **Total**:     138
 - **Passed**:    54
 - **Failed**:    84
 - **Pass Rate**: 39%

## Detailed Results
| Test                                     | Status       | Message                              |
|:-----------------------------------------|:-------------|:-------------------------------------|
| 00_assignment                            | PASS         | Output matches                       |
| 01_comment                               | PASS         | Output matches                       |
| 02_printf                                | PASS         | Output matches                       |
| 03_struct                                | COMPILE_FAIL | rcc returned non-zero                |
| 04_for                                   | PASS         | Output matches                       |
| 05_array                                 | COMPILE_FAIL | rcc returned non-zero                |
| 06_case                                  | PASS         | Output matches                       |
| 07_function                              | COMPILE_FAIL | rcc returned non-zero                |
| 08_while                                 | PASS         | Output matches                       |
| 09_do_while                              | COMPILE_FAIL | rcc returned non-zero                |
| 10_pointer                               | PASS         | Output matches                       |
| 11_precedence                            | COMPILE_FAIL | rcc returned non-zero                |
| 12_hashdefine                            | COMPILE_FAIL | rcc returned non-zero                |
| 13_integer_literals                      | COMPILE_FAIL | rcc returned non-zero                |
| 14_if                                    | PASS         | Output matches                       |
| 15_recursion                             | EXEC_FAIL    | non-zero exit                        |
| 16_nesting                               | PASS         | Output matches                       |
| 17_enum                                  | EXEC_FAIL    | non-zero exit                        |
| 18_include                               | PASS         | Output matches                       |
| 19_pointer_arithmetic                    | COMPILE_FAIL | rcc returned non-zero                |
| 20_pointer_comparison                    | COMPILE_FAIL | rcc returned non-zero                |
| 21_char_array                            | COMPILE_FAIL | rcc returned non-zero                |
| 22_floating_point                        | COMPILE_FAIL | rcc returned non-zero                |
| 23_type_coercion                         | COMPILE_FAIL | rcc returned non-zero                |
| 24_math_library                          | COMPILE_FAIL | rcc returned non-zero                |
| 25_quicksort                             | COMPILE_FAIL | rcc returned non-zero                |
| 26_character_constants                   | PASS         | Output matches                       |
| 27_sizeof                                | PASS         | Output matches                       |
| 28_strings                               | COMPILE_FAIL | rcc returned non-zero                |
| 29_array_address                         | COMPILE_FAIL | rcc returned non-zero                |
| 30_hanoi                                 | COMPILE_FAIL | rcc returned non-zero                |
| 31_args                                  | COMPILE_FAIL | rcc returned non-zero                |
| 32_led                                   | COMPILE_FAIL | rcc returned non-zero                |
| 33_ternary_op                            | COMPILE_FAIL | rcc returned non-zero                |
| 34_array_assignment                      | COMPILE_FAIL | rcc returned non-zero                |
| 35_sizeof                                | COMPILE_FAIL | rcc returned non-zero                |
| 36_array_initialisers                    | COMPILE_FAIL | rcc returned non-zero                |
| 37_sprintf                               | PASS         | Output matches                       |
| 38_multiple_array_index                  | COMPILE_FAIL | rcc returned non-zero                |
| 39_typedef                               | COMPILE_FAIL | rcc returned non-zero                |
| 40_stdio                                 | COMPILE_FAIL | rcc returned non-zero                |
| 41_hashif                                | PASS         | Output matches                       |
| 42_function_pointer                      | EXEC_FAIL    | non-zero exit                        |
| 43_void_param                            | PASS         | Output matches                       |
| 44_scoped_declarations                   | PASS         | Output matches                       |
| 45_empty_for                             | PASS         | Output matches                       |
| 46_grep                                  | COMPILE_FAIL | rcc returned non-zero                |
| 47_switch_return                         | PASS         | Output matches                       |
| 48_nested_break                          | EXEC_FAIL    | non-zero exit                        |
| 49_bracket_evaluation                    | COMPILE_FAIL | rcc returned non-zero                |
| 50_logical_second_arg                    | EXEC_FAIL    | non-zero exit                        |
| 51_static                                | MISMATCH     | Output does not match .expect        |
| 52_unnamed_enum                          | PASS         | Output matches                       |
| 54_goto                                  | PASS         | Output matches                       |
| 55_lshift_type                           | COMPILE_FAIL | rcc returned non-zero                |
| 60_errors_and_warnings                   | SKIP         | Skipped                              |
| 61_integers                              | MISMATCH     | Output does not match .expect        |
| 64_macro_nesting                         | PASS         | Output matches                       |
| 67_macro_concat                          | PASS         | Output matches                       |
| 70_floating_point_literals               | PASS         | Output matches                       |
| 71_macro_empty_arg                       | COMPILE_FAIL | rcc returned non-zero                |
| 72_long_long_constant                    | COMPILE_FAIL | rcc returned non-zero                |
| 73_arm64                                 | COMPILE_FAIL | rcc returned non-zero                |
| 75_array_in_struct_init                  | COMPILE_FAIL | rcc returned non-zero                |
| 76_dollars_in_identifiers                | COMPILE_FAIL | rcc returned non-zero                |
| 77_push_pop_macro                        | PASS         | Output matches                       |
| 78_vla_label                             | SKIP         | Skipped                              |
| 79_vla_continue                          | SKIP         | Skipped                              |
| 80_flexarray                             | COMPILE_FAIL | rcc returned non-zero                |
| 81_types                                 | COMPILE_FAIL | rcc returned non-zero                |
| 82_attribs_position                      | COMPILE_FAIL | rcc returned non-zero                |
| 83_utf8_in_identifiers                   | MISMATCH     | Output does not match .expect        |
| 84_hex-float                             | PASS         | Output matches                       |
| 85_asm-outside-function                  | PASS         | Output matches                       |
| 86_memory-model                          | COMPILE_FAIL | rcc returned non-zero                |
| 87_dead_code                             | COMPILE_FAIL | rcc returned non-zero                |
| 88_codeopt                               | COMPILE_FAIL | rcc returned non-zero                |
| 89_nocode_wanted                         | COMPILE_FAIL | rcc returned non-zero                |
| 90_struct-init                           | COMPILE_FAIL | rcc returned non-zero                |
| 91_ptr_longlong_arith32                  | COMPILE_FAIL | rcc returned non-zero                |
| 92_enum_bitfield                         | COMPILE_FAIL | rcc returned non-zero                |
| 93_integer_promotion                     | COMPILE_FAIL | rcc returned non-zero                |
| 94_generic                               | COMPILE_FAIL | rcc returned non-zero                |
| 95_bitfields                             | COMPILE_FAIL | rcc returned non-zero                |
| 95_bitfields_ms                          | COMPILE_FAIL | rcc returned non-zero                |
| 96_nodata_wanted                         | SKIP         | Skipped                              |
| 97_utf8_string_literal                   | COMPILE_FAIL | rcc returned non-zero                |
| 98_al_ax_extend                          | SKIP         | Skipped                              |
| 99_fastcall                              | SKIP         | Skipped                              |
| 100_c99array-decls                       | COMPILE_FAIL | rcc returned non-zero                |
| 101_cleanup                              | COMPILE_FAIL | rcc returned non-zero                |
| 102_alignas                              | COMPILE_FAIL | rcc returned non-zero                |
| 103_implicit_memmove                     | COMPILE_FAIL | rcc returned non-zero                |
| 104_inline                               | MISMATCH     | Output does not match .expect        |
| 105_local_extern                         | PASS         | Output matches                       |
| 106_versym                               | COMPILE_FAIL | rcc returned non-zero                |
| 107_stack_safe                           | COMPILE_FAIL | rcc returned non-zero                |
| 108_constructor                          | PASS         | Output matches                       |
| 109_float_struct_calling                 | EXEC_FAIL    | non-zero exit                        |
| 110_average                              | COMPILE_FAIL | rcc returned non-zero                |
| 111_conversion                           | PASS         | Output matches                       |
| 112_backtrace                            | SKIP         | Skipped                              |
| 113_btdll                                | SKIP         | Skipped                              |
| 114_bound_signal                         | SKIP         | Skipped                              |
| 115_bound_setjmp                         | SKIP         | Skipped                              |
| 116_bound_setjmp2                        | SKIP         | Skipped                              |
| 117_builtins                             | COMPILE_FAIL | rcc returned non-zero                |
| 118_switch                               | COMPILE_FAIL | rcc returned non-zero                |
| 119_random_stuff                         | COMPILE_FAIL | rcc returned non-zero                |
| 120_alias                                | SKIP         | Skipped                              |
| 121_struct_return                        | COMPILE_FAIL | rcc returned non-zero                |
| 122_vla_reuse                            | SKIP         | Skipped                              |
| 123_vla_bug                              | SKIP         | Skipped                              |
| 124_atomic_counter                       | SKIP         | Skipped                              |
| 125_atomic_misc                          | SKIP         | Skipped                              |
| 126_bound_global                         | SKIP         | Skipped                              |
| 127_asm_goto                             | COMPILE_FAIL | rcc returned non-zero                |
| 128_run_atexit                           | EXEC_FAIL    | non-zero exit                        |
| 129_scopes                               | COMPILE_FAIL | rcc returned non-zero                |
| 130_large_argument                       | COMPILE_FAIL | rcc returned non-zero                |
| 131_return_struct_in_reg                 | COMPILE_FAIL | rcc returned non-zero                |
| 132_bound_test                           | EXEC_FAIL    | non-zero exit                        |
| 133_old_func                             | MISMATCH     | Output does not match .expect        |
| 134_double_to_signed                     | MISMATCH     | Output does not match .expect        |
| 135_func_arg_struct_compare              | COMPILE_FAIL | rcc returned non-zero                |
| 136_atomic_gcc_style                     | SKIP         | Skipped                              |
| 137_funcall_struct_args                  | MISMATCH     | Output does not match .expect        |
| test_bitfields                           | COMPILE_FAIL | rcc returned non-zero                |
| test_builtins                            | COMPILE_FAIL | rcc returned non-zero                |
| test_elif2                               | PASS         | exit=80                              |
| test_elif_simple                         | PASS         | exit=80                              |
| test_err                                 | PASS         | compile error as expected            |
| test_func                                | PASS         | exit=0                               |
| test_if                                  | PASS         | exit=0                               |
| test_if2                                 | PASS         | exit=0                               |
| test_if3                                 | PASS         | exit=0                               |
| test_if4                                 | PASS         | exit=0                               |
| test_if5                                 | PASS         | exit=80                              |
| test_if6                                 | PASS         | exit=80                              |
| test_if_nested                           | PASS         | exit=80                              |
| test_if_simple                           | PASS         | exit=80                              |
| test_include                             | PASS         | exit=80                              |
| test_include2                            | PASS         | exit=80                              |
| test_loop                                | PASS         | exit=0                               |
| test_macro                               | PASS         | exit=1                               |
| test_minimal                             | PASS         | exit=0                               |
| test_nested_if                           | PASS         | exit=0                               |
| test_ptr                                 | COMPILE_FAIL | rcc returned non-zero                |
| test_real                                | PASS         | exit=0                               |
| test_self_include2                       | PASS         | exit=80                              |
| test_signextend                          | COMPILE_FAIL | rcc returned non-zero                |
| test_simple                              | PASS         | exit=80                              |
| test_simple2                             | PASS         | exit=80                              |
| test_str                                 | PASS         | exit=0                               |
| test_struct                              | PASS         | exit=0                               |
| test_with_comment                        | PASS         | exit=0                               |
