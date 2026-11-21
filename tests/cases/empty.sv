module empty;
`ifdef CHECK_CONSTANT
    always_comb p_false: assert (1'b0);
`endif
endmodule
