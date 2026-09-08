// MODULE 4 (Jarvis bridge) OWNS THIS FILE. File scope seam, see include/dai_ext.h.
//
// Included once by every host that can be driven from outside:
// examples/editor_demo.cpp and tools/modeling_shot.cpp. It is the local TCP
// socket an outside tool (tools/daibridge.py, or Jarvis) talks to.
//
// The contract, and none of it is negotiable:
//
//   * OFF by default. It opens only when the host was told to - a flag on the
//     command line or a project setting - and it says in the console that it
//     is open and on which port.
//   * 127.0.0.1 ONLY. Bound to the loopback address, never to 0.0.0.0. A
//     modelling socket that answers the network is a remote code execution
//     hole, because "run this JS" is exactly what it is for.
//   * One JSON object per line, in and out. No length prefixes, no framing to
//     get wrong, and a human can talk to it with netcat.
//   * Everything it changes goes through the SAME path the panels use:
//     dai_doc_begin/commit around each command, the existing JS API for the
//     work itself. Undo after a bridge command undoes exactly that command.
//   * Non blocking. A bridge with nobody connected costs one poll per frame;
//     a client that stops reading must not freeze the editor.
//
// Called once per frame, near the end, after the document has settled.

static void dai_bridge_host_poll(const dai_ext_host *h) {
    (void)h;   /* module 4 fills this in */
}
