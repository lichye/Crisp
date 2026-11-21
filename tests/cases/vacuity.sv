module vacuity(input bit clk, rst, request, blocked);
    bit [3:0] count = 0;
    always_ff @(posedge clk) if (count < 6) count <= count + 1'b1;

    idle: assume property (@(posedge clk) !blocked);
    basic_constant_false: assert property (@(posedge clk) (request && !request) |-> !request);
    basic_never: assert property (@(posedge clk) (count == 9) |-> (count == 9));
    basic_live: assert property (@(posedge clk) request |-> request);
    late: assert property (@(posedge clk) (count == 6) |-> (count != 0));
    basic_disabled: assert property (@(posedge clk) disable iff (rst) rst |-> request);
    basic_assumed_out: assert property (@(posedge clk) blocked |-> request);
    next_dead: assert property (@(posedge clk) blocked |=> request);
    next_live: assert property (@(posedge clk) request |=> count <= 6);
    basic_bool_dead: assert property (@(posedge clk) (blocked -> request));
    basic_bool_live: assert property (@(posedge clk) (request -> !blocked));
    chained: assert property (@(posedge clk) request |-> (blocked |-> request));
    basic_ordinary: assert property (@(posedge clk) count <= 6);
    wrong: assert property (@(posedge clk) request |-> !request);
    double_reset: assert property (@(posedge clk) (rst ##1 rst) |=> request);
    last_sample: assert property (@(posedge clk) (count == 2 ##1 count == 3) |=> ##2 1'b1);
endmodule

module sampled(input bit clk, rst);
    // Reset is always witnessed at the initial sample. A second target keeps
    // the sampler running after it has published that first witness.
    bit [4:0] cycles = 0;
    always_ff @(posedge clk) if (cycles < 30) cycles <= cycles + 1'b1;
    after30: assert property (@(posedge clk) (cycles == 30) |-> (cycles != 0));
    random_seen: assert property (@(posedge clk) rst |=> 1'b1);
    random_never: assert property (@(posedge clk) (rst ##1 rst) |=> 1'b1);
endmodule

module assumption_cone(input bit clk, request);
    bit delayed = 0;
    always_ff @(posedge clk) delayed <= request;
    quiet: assume property (@(posedge clk) !delayed);
    // Only the assumption mentions delayed. Its next-state definition is
    // essential: a sampled request rules out a valid following sample.
    dead: assert property (@(posedge clk) (request ##1 1'b1) |=> 1'b1);
endmodule
