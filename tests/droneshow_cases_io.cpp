// Stages 5 and 6 under test: a planted collision, and a lossless round trip.
//
// Implements show_cases_io() - see tests/droneshow_cases.hpp.
//
//   [5a] THE PLANTED COLLISION: build a plan with dai_show_plan_from_keys in
//        which two drones are put 0.5 m apart at a known time. The validator
//        must find it, at that time, naming those two drones, with the right
//        shortfall in metres. A validator that finds nothing is the failure
//        mode this whole panel exists to prevent.
//   [5b] a clean plan yields zero conflicts - the other half of [5a], because
//        a validator that reports everything is equally useless.
//   [5c] planted v_max, a_max, geofence and ground violations are each found
//        and reported with the right kind.
//   [5d] the conflict list is sorted by (time, a, b) and identical across two
//        runs of the same input.
//   [5e] the broadphase is used: pairs_tested per tick is far below n^2/2 on a
//        sparse formation, and the answer is the same as an O(n^2) reference
//        check computed here on a small instance.
//   [6a] JSON export -> import is LOSSLESS: same drone count, same keyframe
//        count per drone, same times, positions and colours, bit for bit.
//   [6b] .skyc export -> import is lossless in the same sense, and the file it
//        writes is a readable ZIP whose show.json parses.
//   [6c] CSV has one line per drone per frame, the header names the columns
//        the header file promises, and the values match dai_show_plan_sample.
//   [6d] an interrupted export leaves no file behind (write to temp, rename).
#include "droneshow_cases.hpp"
