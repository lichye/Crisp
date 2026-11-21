// IEEE 1800-2017 20.10: the first argument of $fatal is the finish number 0, 1 or 2.
// slang 11.0 reports a string there only as a warning (-Wfinish-num).
module fatal_string(input clk, input [3:0] a);
    always_ff @(posedge clk) if (a == 4'hf) $fatal("a reached 15");
    p_true: assert property (@(posedge clk) a == a);
endmodule
