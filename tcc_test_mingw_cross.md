# TCC Test Suite Report for RCC
Generated: April 2026

## Summary
 - **Total**:     135
 - **Passed**:    128
 - **Failed**:    7
 - **Pass Rate**: 94%

## Detailed Results
| Test                                     | Status       | Message                              |
|:-----------------------------------------|:-------------|:-------------------------------------|
| 00_assignment                            | PASS         | Output matches                       |
| 01_comment                               | PASS         | Output matches                       |
| 02_printf                                | PASS         | Output matches                       |
| 03_struct                                | PASS         | Output matches                       |
| 04_for                                   | PASS         | Output matches                       |
| 05_array                                 | PASS         | Output matches                       |
| 06_case                                  | PASS         | Output matches                       |
| 07_function                              | PASS         | Output matches                       |
| 08_while                                 | PASS         | Output matches                       |
| 09_do_while                              | PASS         | Output matches                       |
| 10_pointer                               | PASS         | Output matches                       |
| 11_precedence                            | PASS         | Output matches                       |
| 12_hashdefine                            | PASS         | Output matches                       |
| 13_integer_literals                      | PASS         | Output matches                       |
| 14_if                                    | PASS         | Output matches                       |
| 15_recursion                             | PASS         | Output matches                       |
| 16_nesting                               | PASS         | Output matches                       |
| 17_enum                                  | PASS         | Output matches                       |
| 18_include                               | PASS         | Output matches                       |
| 19_pointer_arithmetic                    | PASS         | Output matches                       |
| 20_pointer_comparison                    | PASS         | Output matches                       |
| 21_char_array                            | PASS         | Output matches                       |
| 22_floating_point                        | PASS         | Output matches                       |
| 23_type_coercion                         | PASS         | Output matches                       |
| 24_math_library                          | PASS         | Output matches                       |
| 25_quicksort                             | PASS         | Output matches                       |
| 26_character_constants                   | PASS         | Output matches                       |
| 27_sizeof                                | PASS         | Output matches                       |
| 28_strings                               | PASS         | Output matches                       |
| 29_array_address                         | PASS         | Output matches                       |
| 30_hanoi                                 | PASS         | Output matches                       |
| 31_args                                  | PASS         | Output matches                       |
| 32_led                                   | PASS         | Output matches                       |
| 33_ternary_op                            | PASS         | Output matches                       |
| 34_array_assignment                      | PASS         | Output matches                       |
| 35_sizeof                                | PASS         | Output matches                       |
| 36_array_initialisers                    | PASS         | Output matches                       |
| 37_sprintf                               | PASS         | Output matches                       |
| 38_multiple_array_index                  | PASS         | Output matches                       |
| 39_typedef                               | PASS         | Output matches                       |
| 40_stdio                                 | PASS         | Output matches                       |
| 41_hashif                                | PASS         | Output matches                       |
| 42_function_pointer                      | PASS         | Output matches                       |
| 43_void_param                            | PASS         | Output matches                       |
| 44_scoped_declarations                   | PASS         | Output matches                       |
| 45_empty_for                             | PASS         | Output matches                       |
| 46_grep                                  | EXEC_FAIL    | non-zero exit                        |
| 47_switch_return                         | PASS         | Output matches                       |
| 48_nested_break                          | PASS         | Output matches                       |
| 49_bracket_evaluation                    | PASS         | Output matches                       |
| 50_logical_second_arg                    | PASS         | Output matches                       |
| 51_static                                | PASS         | Output matches                       |
| 52_unnamed_enum                          | PASS         | Output matches                       |
| 54_goto                                  | PASS         | Output matches                       |
| 55_lshift_type                           | PASS         | Output matches                       |
| 60_errors_and_warnings                   | SKIP         | Skipped                              |
| 61_integers                              | PASS         | Output matches                       |
| 64_macro_nesting                         | PASS         | Output matches                       |
| 67_macro_concat                          | PASS         | Output matches                       |
| 70_floating_point_literals               | PASS         | Output matches                       |
| 71_macro_empty_arg                       | PASS         | Output matches                       |
| 72_long_long_constant                    | PASS         | Output matches                       |
| 73_arm64                                 | SKIP         | Skipped                              |
| 75_array_in_struct_init                  | PASS         | Output matches                       |
| 76_dollars_in_identifiers                | PASS         | Output matches                       |
| 77_push_pop_macro                        | PASS         | Output matches                       |
| 78_vla_label                             | SKIP         | Skipped                              |
| 79_vla_continue                          | SKIP         | Skipped                              |
| 80_flexarray                             | PASS         | Output matches                       |
| 81_types                                 | PASS         | Output matches                       |
| 82_attribs_position                      | PASS         | Output matches                       |
| 83_utf8_in_identifiers                   | PASS         | Output matches                       |
| 84_hex-float                             | PASS         | Output matches                       |
| 85_asm-outside-function                  | PASS         | Output matches                       |
| 86_memory-model                          | PASS         | Output matches                       |
| 87_dead_code                             | PASS         | Output matches                       |
| 88_codeopt                               | PASS         | Output matches                       |
| 89_nocode_wanted                         | PASS         | Output matches                       |
| 90_struct-init                           | PASS         | Output matches                       |
| 91_ptr_longlong_arith32                  | PASS         | Output matches                       |
| 92_enum_bitfield                         | PASS         | Output matches                       |
| 93_integer_promotion                     | PASS         | Output matches                       |
| 94_generic                               | PASS         | Output matches                       |
| 95_bitfields                             | SKIP         | Skipped                              |
| 95_bitfields_ms                          | MISMATCH     | Output does not match .expect        |
| 96_nodata_wanted                         | SKIP         | Skipped                              |
| 97_utf8_string_literal                   | PASS         | Output matches                       |
| 98_al_ax_extend                          | SKIP         | Skipped                              |
| 99_fastcall                              | SKIP         | Skipped                              |
| 100_c99array-decls                       | PASS         | Output matches                       |
| 101_cleanup                              | PASS         | Output matches                       |
| 102_alignas                              | PASS         | Output matches                       |
| 103_implicit_memmove                     | PASS         | Output matches                       |
| 104_inline                               | PASS         | Output matches                       |
| 105_local_extern                         | PASS         | Output matches                       |
| 106_versym                               | COMPILE_FAIL | rcc returned non-zero                |
| 107_stack_safe                           | PASS         | Output matches                       |
| 108_constructor                          | PASS         | Output matches                       |
| 109_float_struct_calling                 | MISMATCH     | Output does not match .expect        |
| 110_average                              | PASS         | Output matches                       |
| 111_conversion                           | PASS         | Output matches                       |
| 112_backtrace                            | SKIP         | Skipped                              |
| 113_btdll                                | SKIP         | Skipped                              |
| 114_bound_signal                         | SKIP         | Skipped                              |
| 115_bound_setjmp                         | SKIP         | Skipped                              |
| 116_bound_setjmp2                        | SKIP         | Skipped                              |
| 117_builtins                             | PASS         | Output matches                       |
| 118_switch                               | PASS         | Output matches                       |
| 119_random_stuff                         | PASS         | Output matches                       |
| 120_alias                                | SKIP         | Skipped                              |
| 121_struct_return                        | PASS         | Output matches                       |
| 122_vla_reuse                            | SKIP         | Skipped                              |
| 123_vla_bug                              | SKIP         | Skipped                              |
| 124_atomic_counter                       | SKIP         | Skipped                              |
| 125_atomic_misc                          | SKIP         | Skipped                              |
| 126_bound_global                         | SKIP         | Skipped                              |
| 127_asm_goto                             | PASS         | Output matches                       |
| 128_run_atexit                           | COMPILE_FAIL | executable missing                   |
| 129_scopes                               | COMPILE_FAIL | rcc returned non-zero                |
| 130_large_argument                       | MISMATCH     | Output does not match .expect        |
| 131_return_struct_in_reg                 | PASS         | Output matches                       |
| 132_bound_test                           | PASS         | Output matches                       |
| 133_old_func                             | PASS         | Output matches                       |
| 134_double_to_signed                     | PASS         | Output matches                       |
| 135_func_arg_struct_compare              | PASS         | Output matches                       |
| 136_atomic_gcc_style                     | SKIP         | Skipped                              |
| 137_funcall_struct_args                  | PASS         | Output matches                       |
| test_bitfields                           | PASS         | exit=0                               |
| test_elif2                               | PASS         | exit=0                               |
| test_elif_simple                         | PASS         | exit=0                               |
| test_err                                 | PASS         | compile error as expected            |
| test_func                                | PASS         | exit=0                               |
| test_if                                  | PASS         | exit=0                               |
| test_if2                                 | PASS         | exit=0                               |
| test_if3                                 | PASS         | exit=0                               |
| test_if4                                 | PASS         | exit=0                               |
| test_if5                                 | PASS         | exit=0                               |
| test_if6                                 | PASS         | exit=0                               |
| test_if_nested                           | PASS         | exit=0                               |
| test_if_simple                           | PASS         | exit=0                               |
| test_include                             | PASS         | exit=42                              |
| test_include2                            | PASS         | exit=10                              |
| test_loop                                | PASS         | exit=0                               |
| test_macro                               | PASS         | exit=1                               |
| test_minimal                             | PASS         | exit=0                               |
| test_nested_if                           | PASS         | exit=0                               |
| test_ptr                                 | PASS         | exit=0                               |
| test_real                                | PASS         | exit=0                               |
| test_self_include2                       | PASS         | exit=1                               |
| test_signextend                          | PASS         | exit=5                               |
| test_simple                              | PASS         | exit=1                               |
| test_simple2                             | PASS         | exit=1                               |
| test_str                                 | PASS         | exit=0                               |
| test_struct                              | PASS         | exit=0                               |
| test_with_comment                        | PASS         | exit=0                               |
