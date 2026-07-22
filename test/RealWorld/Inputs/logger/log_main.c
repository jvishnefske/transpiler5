// RealWorld corpus (Track 4), multi-TU program `logger`: the driver TU.
// Calls the logger defined in log_impl.c across the TU boundary.
void log_msg(const char *);

int main(void) {
  log_msg("start");
  log_msg("processing");
  log_msg("done");
  return 0;
}
