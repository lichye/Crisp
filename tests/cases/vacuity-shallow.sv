module shallow(input bit clk, rst, advance, ready, trip, alarm);
    bit [4:0] count = 0;
    always_ff @(posedge clk) if (advance && count < 30) count <= count + 1'b1;

    quiet: assume property (@(posedge clk) rst |=> !ready);
    // The contradiction cannot be observed on a four-cycle prefix.
    future: assume property (@(posedge clk) trip |-> ##8 alarm);
`ifndef ALLOW_ALARM
    no_alarm: assume property (@(posedge clk) !alarm);
`endif

    early: assert property (@(posedge clk) (rst ##1 !ready) |=> 1'b1);
    late: assert property (@(posedge clk) (count == 30) |-> (count != 0));
    excluded: assert property (@(posedge clk) (rst ##1 ready) |=> 1'b1);
    outstanding: assert property (@(posedge clk) trip |-> 1'b1);
endmodule

module periodic(input bit clk, rst);
    bit phase = 0;
    bit [2:0] age = 0;
    always_ff @(posedge clk) begin
        phase <= !phase;
        if (age < 5) age <= age + 1'b1;
    end
    starts_high: assume property (@(posedge clk) rst |=> phase);
    on_time: assert property (@(posedge clk) (age == 4) |-> !phase);
    later: assert property (@(posedge clk) (age == 5) |-> age != 0);
endmodule

// The only match is at the initial sample, but the first loop comes after
// age reaches 24. Preserve the suffix of the original engine's strict witness.
module longloop(input bit clk, rst, start);
    bit [4:0] age = 0;
    always_ff @(posedge clk) if (age < 24) age <= age + 1'b1;
    initial_start: assume property (@(posedge clk) (age == 0) |-> start);
    hit: assert property (@(posedge clk) (age == 0 && start) |-> 1'b1);
endmodule
