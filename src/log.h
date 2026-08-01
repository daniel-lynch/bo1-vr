/* log.h -- stderr logging channel.
 *
 * Proton redirects the game's stderr into ~/steam-<appid>.log (steam-42700.log
 * for Black Ops). That makes fprintf(stderr, ...) a zero-setup logging channel
 * that survives crashes, needs no console allocation, and costs nothing to
 * read. Given the state of 32-bit debugging under new-WoW64 (see
 * experiments/03_winedbg), this is the primary diagnostic channel, not a
 * fallback.
 *
 * stderr is unbuffered-ish under Wine but we fflush explicitly: a crash in the
 * game must not eat the last line we wrote, which is usually the interesting one.
 */
#ifndef BO1VR_LOG_H
#define BO1VR_LOG_H

void bo1vr_log(const char *fmt, ...);

#define LOGI(...) bo1vr_log("[bo1-vr] " __VA_ARGS__)
#define LOGE(...) bo1vr_log("[bo1-vr][error] " __VA_ARGS__)

#endif /* BO1VR_LOG_H */
