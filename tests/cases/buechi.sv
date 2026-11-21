module buechi(input logic clk, rst, ready, choose_next);
`ifndef NO_CONTRACT
    contract: assume property (@(posedge clk) rst |=> !ready);
`endif
`ifdef LIVE_CONTRACT
    live: assume property (@(posedge clk) s_eventually ready);
`endif
    p_quiet: assert property (@(posedge clk) rst |=> !ready);
    // Each witness falsifies exactly one of these targets after the initial reset.
    bit started = 0;
    always @(posedge clk) started <= 1;
    initial_reset_only: assume property (@(posedge clk) started |-> !rst);
    p_next: assert property (@(posedge clk) (rst && choose_next) |=> ready);
    p_later: assert property (@(posedge clk) (rst && !choose_next) |-> ##2 ready);
endmodule
