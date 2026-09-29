void loop(void) {
  // This comment explains the loop
  // below. It was wrapped by hand at a
  // short width, so each line is ragged
  // and nothing reflows it.
  for (;;) {
    // A deeper comment is its own run
    // and refills at its own indent.
    // Its lines join.
  // A shallower line after it starts
  // another run.
  }
  // Code between two comments
  loop();
  // keeps them apart.

  // A blank line

  // keeps them apart too.
  int x; // A trailing comment stays exactly as written.
  // An indented list keeps its items:
  // - first item
  // - second item
}

void tabs(void) {
	// A tab indent and eight spaces
	// share a column, so these
	// lines form one run.
	if (1) {
		// Two tabs match a tab
		// and eight spaces, so
		// these lines join as
		// well. Sixteen spaces
		// match them too.
	                // A tab and
	                // sixteen
	                // spaces sit
	                // deeper and
	                // start a new
	                // run.
	}
}
