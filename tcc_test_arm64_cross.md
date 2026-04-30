# TCC Test Suite Report for RCC
Generated: May 2026

## Summary
 - **Total**:     142
 - **Passed**:    86
 - **Failed**:    56
 - **Pass Rate**: 60%

## Detailed Results
| Test                                     | Status       | Message                              |
|:-----------------------------------------|:-------------|:-------------------------------------|
| 00_assignment                            | PASS         | Output matches                       |
| 01_comment                               | PASS         | Output matches                       |
| 02_printf                                | PASS         | Output matches                       |
| 03_struct                                | PASS         | Output matches                       |
| 04_for                                   | PASS         | Output matches                       |
| 05_array                                 | MISMATCH     | Output does not match .expect        |
| 06_case                                  | PASS         | Output matches                       |
| 07_function                              | PASS         | Output matches                       |
| 08_while                                 | PASS         | Output matches                       |
| 09_do_while                              | PASS         | Output matches                       |
| 10_pointer                               | PASS         | Output matches                       |
| 11_precedence                            | PASS         | Output matches                       |
| 12_hashdefine                            | PASS         | Output matches                       |
| 13_integer_literals                      | PASS         | Output matches                       |
| 14_if                                    | PASS         | Output matches                       |
| 15_recursion                             | EXEC_FAIL    | non-zero exit                        |
| 16_nesting                               | PASS         | Output matches                       |
| 17_enum                                  | PASS         | Output matches                       |
| 18_include                               | PASS         | Output matches                       |
| 19_pointer_arithmetic                    | PASS         | Output matches                       |
| 20_pointer_comparison                    | PASS         | Output matches                       |
| 21_char_array                            | EXEC_FAIL    | non-zero exit                        |
| 22_floating_point                        | MISMATCH     | Output does not match .expect        |
| 23_type_coercion                         | MISMATCH     | Output does not match .expect        |
| 24_math_library                          | MISMATCH     | Output does not match .expect        |
| 25_quicksort                             | EXEC_FAIL    | non-zero exit                        |
| 26_character_constants                   | PASS         | Output matches                       |
| 27_sizeof                                | PASS         | Output matches                       |
| 28_strings                               | PASS         | Output matches                       |
| 29_array_address                         | PASS         | Output matches                       |
| 30_hanoi                                 | EXEC_FAIL    | non-zero exit                        |
| 31_args                                  | PASS         | Output matches                       |
| 32_led                                   | EXEC_FAIL    | non-zero exit                        |
| 33_ternary_op                            | MISMATCH     | Output does not match .expect        |
| 34_array_assignment                      | MISMATCH     | Output does not match .expect        |
| 35_sizeof                                | PASS         | Output matches                       |
| 36_array_initialisers                    | PASS         | Output matches                       |
| 37_sprintf                               | PASS         | Output matches                       |
| 38_multiple_array_index                  | PASS         | Output matches                       |
| 39_typedef                               | MISMATCH     | Output does not match .expect        |
| 40_stdio                                 | EXEC_FAIL    | non-zero exit                        |
| 41_hashif                                | PASS         | Output matches                       |
| 42_function_pointer                      | EXEC_FAIL    | non-zero exit                        |
| 43_void_param                            | PASS         | Output matches                       |
| 44_scoped_declarations                   | PASS         | Output matches                       |
| 45_empty_for                             | PASS         | Output matches                       |
| 46_grep                                  | EXEC_FAIL    | non-zero exit                        |
| 47_switch_return                         | PASS         | Output matches                       |
| 48_nested_break                          | EXEC_FAIL    | non-zero exit                        |
| 49_bracket_evaluation                    | PASS         | Output matches                       |
| 50_logical_second_arg                    | PASS         | Output matches                       |
| 51_static                                | MISMATCH     | Output does not match .expect        |
| 52_unnamed_enum                          | PASS         | Output matches                       |
| 54_goto                                  | PASS         | Output matches                       |
| 55_lshift_type                           | EXEC_FAIL    | non-zero exit                        |
| 60_errors_and_warnings                   | SKIP         | Skipped                              |
| 61_integers                              | MISMATCH     | Output does not match .expect        |
| 64_macro_nesting                         | PASS         | Output matches                       |
| 67_macro_concat                          | MISMATCH     | Output does not match .expect        |
| 70_floating_point_literals               | PASS         | Output matches                       |
| 71_macro_empty_arg                       | MISMATCH     | Output does not match .expect        |
| 72_long_long_constant                    | PASS         | Output matches                       |
| 73_arm64                                 | COMPILE_FAIL | rcc returned non-zero                |
| 75_array_in_struct_init                  | PASS         | Output matches                       |
| 76_dollars_in_identifiers                | PASS         | Output matches                       |
| 77_push_pop_macro                        | PASS         | Output matches                       |
| 78_vla_label                             | COMPILE_FAIL | rcc returned non-zero                |
| 79_vla_continue                          | COMPILE_FAIL | rcc returned non-zero                |
| 80_flexarray                             | EXEC_FAIL    | non-zero exit                        |
| 81_types                                 | PASS         | Output matches                       |
| 82_attribs_position                      | MISMATCH     | Output does not match .expect        |
| 83_utf8_in_identifiers                   | MISMATCH     | Output does not match .expect        |
| 84_hex-float                             | MISMATCH     | Output does not match .expect        |
| 85_asm-outside-function                  | PASS         | Output matches                       |
| 86_memory-model                          | PASS         | Output matches                       |
| 87_dead_code                             | EXEC_FAIL    | non-zero exit                        |
| 88_codeopt                               | MISMATCH     | Output does not match .expect        |
| 89_nocode_wanted                         | PASS         | Output matches                       |
| 90_struct-init                           | COMPILE_FAIL | rcc returned non-zero                |
| 91_ptr_longlong_arith32                  | PASS         | Output matches                       |
| 92_enum_bitfield                         | MISMATCH     | Output does not match .expect        |
| 93_integer_promotion                     | MISMATCH     | Output does not match .expect        |
| 94_generic                               | PASS         | Output matches                       |
| 95_bitfields                             | COMPILE_FAIL | rcc returned non-zero                |
| 95_bitfields_ms                          | COMPILE_FAIL | rcc returned non-zero                |
| 96_nodata_wanted                         | SKIP         | Skipped                              |
| 97_utf8_string_literal                   | MISMATCH     | Output does not match .expect        |
| 98_al_ax_extend                          | SKIP         | Skipped                              |
| 99_fastcall                              | SKIP         | Skipped                              |
| 100_c99array-decls                       | PASS         | Output matches                       |
| 101_cleanup                              | COMPILE_FAIL | rcc returned non-zero                |
| 102_alignas                              | PASS         | Output matches                       |
| 103_implicit_memmove                     | PASS         | Output matches                       |
| 104_inline                               | MISMATCH     | Output does not match .expect        |
| 105_local_extern                         | PASS         | Output matches                       |
| 106_versym                               | COMPILE_FAIL | rcc returned non-zero                |
| 107_stack_safe                           | PASS         | Output matches                       |
| 108_constructor                          | PASS         | Output matches                       |
| 109_float_struct_calling                 | EXEC_FAIL    | non-zero exit                        |
| 110_average                              | MISMATCH     | Output does not match .expect        |
| 111_conversion                           | PASS         | Output matches                       |
| 112_backtrace                            | SKIP         | Skipped                              |
| 113_btdll                                | SKIP         | Skipped                              |
| 114_bound_signal                         | SKIP         | Skipped                              |
| 115_bound_setjmp                         | SKIP         | Skipped                              |
| 116_bound_setjmp2                        | SKIP         | Skipped                              |
| 117_builtins                             | MISMATCH     | Output does not match .expect        |
| 118_switch                               | COMPILE_FAIL | rcc returned non-zero                |
| 119_random_stuff                         | COMPILE_FAIL | rcc returned non-zero                |
| 120_alias                                | SKIP         | Skipped                              |
| 121_struct_return                        | EXEC_FAIL    | non-zero exit                        |
| 122_vla_reuse                            | COMPILE_FAIL | rcc returned non-zero                |
| 123_vla_bug                              | COMPILE_FAIL | rcc returned non-zero                |
| 124_atomic_counter                       | SKIP         | Skipped                              |
| 125_atomic_misc                          | SKIP         | Skipped                              |
| 126_bound_global                         | SKIP         | Skipped                              |
| 127_asm_goto                             | COMPILE_FAIL | rcc returned non-zero                |
| 128_run_atexit                           | EXEC_FAIL    | non-zero exit                        |
| 129_scopes                               | MISMATCH     | Output does not match .expect        |
| 130_large_argument                       | COMPILE_FAIL | rcc returned non-zero                |
| 131_return_struct_in_reg                 | EXEC_FAIL    | non-zero exit                        |
| 132_bound_test                           | PASS         | Output matches                       |
| 133_old_func                             | MISMATCH     | Output does not match .expect        |
| 134_double_to_signed                     | MISMATCH     | Output does not match .expect        |
| 135_func_arg_struct_compare              | PASS         | Output matches                       |
| 136_atomic_gcc_style                     | SKIP         | Skipped                              |
| 137_funcall_struct_args                  | MISMATCH     | Output does not match .expect        |
| test_bitfields                           | PASS         | exit=0                               |
| test_builtins                            | COMPILE_FAIL | rcc returned non-zero                |
| test_elif2                               | PASS         | exit=96                              |
| test_elif_simple                         | PASS         | exit=96                              |
| test_err                                 | PASS         | compile error as expected            |
| test_func                                | PASS         | exit=0                               |
| test_if                                  | PASS         | exit=0                               |
| test_if2                                 | PASS         | exit=0                               |
| test_if3                                 | PASS         | exit=0                               |
| test_if4                                 | PASS         | exit=0                               |
| test_if5                                 | PASS         | exit=96                              |
| test_if6                                 | PASS         | exit=96                              |
| test_if_nested                           | PASS         | exit=96                              |
| test_if_simple                           | PASS         | exit=96                              |
| test_include                             | PASS         | exit=96                              |
| test_include2                            | PASS         | exit=96                              |
| test_loop                                | PASS         | exit=124                             |
| test_macro                               | PASS         | exit=1                               |
| test_minimal                             | PASS         | exit=0                               |
| test_nested_if                           | PASS         | exit=0                               |
| test_ptr                                 | PASS         | exit=0                               |
| test_real                                | PASS         | exit=0                               |
| test_self_include2                       | PASS         | exit=96                              |
| test_signextend                          | PASS         | exit=0                               |
| test_simple                              | PASS         | exit=96                              |
| test_simple2                             | PASS         | exit=96                              |
| test_str                                 | PASS         | exit=0                               |
| test_struct                              | PASS         | exit=0                               |
| test_with_comment                        | PASS         | exit=0                               |
