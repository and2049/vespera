// Compiles the pinned audio-io-core engine with its platform context factory renamed, so that the
// scripted test backend can provide ae_context_create while every other entry point, including
// stream validation, dispatch, fault latching and status reporting, remains upstream code.
#define ae_context_create ae_pinned_context_create
#include "engine.cpp"
