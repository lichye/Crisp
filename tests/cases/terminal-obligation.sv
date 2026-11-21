module tail(input bit clk, trip, alarm);
`ifdef ONE_CYCLE
  future: assume property (@(posedge clk) trip |=> alarm);
`else
  future: assume property (@(posedge clk) trip |-> ##8 alarm);
`endif
`ifndef ALLOW_ALARM
  quiet: assume property (@(posedge clk) !alarm);
`endif
  unreachable: assert property (@(posedge clk) !trip);
  match_trip: assert property (@(posedge clk) trip |-> 1'b1);
endmodule
