module lasso_alarm(input logic clk, trip, alarm);
  logic seen = 1'b0;
  always @(posedge clk)
    seen <= trip;

  a_alarm: assume property (@(posedge clk) trip |=> alarm);
  p_no_trip: assert property (@(posedge clk) !seen);
endmodule
