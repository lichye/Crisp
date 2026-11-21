module buechi(input logic clk, rst, ready);
`ifndef NO_CONTRACT
    contract: assume property (@(posedge clk) rst |=> !ready);
`endif
`ifdef LIVE_CONTRACT
    live: assume property (@(posedge clk) s_eventually ready);
`endif
    p_quiet: assert property (@(posedge clk) rst |=> !ready);
    p_next: assert property (@(posedge clk) rst |=> ready);
    p_later: assert property (@(posedge clk) rst |-> ##2 ready);
endmodule
