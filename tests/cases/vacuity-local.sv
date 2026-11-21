module guarded_child(input bit clk, blocked, value);
    property capture;
        bit saved;
        @(posedge clk) (blocked, saved = value) |=> value == saved;
    endproperty
    captured: assert property (capture);
    overlap: assert property (@(posedge clk) blocked |-> value);
    boolean_arrow: assert property (@(posedge clk) (blocked -> value));
    next_cycle: assert property (@(posedge clk) blocked |=> value);
endmodule

module guarded_top(input bit clk, req, value);
    idle: assume property (@(posedge clk) !req);
    guarded_child child(.clk(clk), .blocked(req), .value(value));
endmodule
