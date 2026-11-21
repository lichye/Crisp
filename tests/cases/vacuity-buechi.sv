module vacuity_leaf(input bit clk, rst, ready, ack);
    valid_ack: assume property (@(posedge clk) ack);
    blocked: assert property (@(posedge clk) (rst ##1 ready) |-> ack);
    property captured_req;
        bit saved;
        @(posedge clk) rst ##1 (ready, saved = ack) |=> ack == saved;
    endproperty
    captured: assert property (captured_req);
    allowed: assert property (@(posedge clk) (rst ##1 !ready) |-> ack);
endmodule

module vacuity_monitor(input bit clk, rst, ready, ack);
`ifndef NO_CONTRACT
    quiet: assume property (@(posedge clk) rst |=> !ready);
`endif
    vacuity_leaf leaf(.clk(clk), .rst(rst), .ready(ready), .ack(ack));
    next_bad: assert property (@(posedge clk) rst |=> ready);
    later_bad: assert property (@(posedge clk) rst |-> ##2 ready);
endmodule
