module tail(input clk, trip, alarm);
`ifdef RTL_MONITOR
    bit owed = 0;
    always @(posedge clk) owed <= trip;
    a1: assume property (@(posedge clk) !owed || alarm);
`else
    a1: assume property (@(posedge clk) trip |=> alarm);
`endif
`ifndef ALLOW_ALARM
    a2: assume property (@(posedge clk) !alarm);
`endif
    ordinary: assert property (@(posedge clk) !trip);
    match_trip: assert property (@(posedge clk) trip |-> 1'b1);
endmodule
