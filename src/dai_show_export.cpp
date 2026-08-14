// Stage 6: the show leaves the building.
//
// Implements, from include/dai_show.h:
//     dai_show_export_skyc / dai_show_import_skyc
//     dai_show_export_csv
//     dai_show_export_json / dai_show_import_json
//
// .skyc is Skybrush's container and it is the reason this exporter exists at
// all: the shows have to fly on firmware that has been through a thousand
// displays, not on something written here last month. The format is a ZIP with
// a show.json and one trajectory and light program per drone. It is
// implemented from the format description, NOT ported - Daidalos is MIT, the
// reference implementation is GPL, and the two do not mix. The ZIP writer and
// the DEFLATE store path live here rather than in the engine because nothing
// else in Daidalos writes an archive.
//
// CSV and JSON exist because a container only one program can open makes a
// show unauditable. The JSON one round trips exactly - keyframes in, the same
// keyframes out - and the test proves it by comparing the reimported plan
// against the original key for key.
