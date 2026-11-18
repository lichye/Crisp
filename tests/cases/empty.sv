module empty;
`ifdef CHECK_CONSTANT
    p_false: assert property (1'b0);
`endif
endmodule
