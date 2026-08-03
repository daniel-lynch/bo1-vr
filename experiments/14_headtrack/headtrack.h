/* headtrack.h -- HMD orientation -> refdef.viewaxis, and the checks that keep
 * it honest.  (BAC-282 follow-on to exp 13's position-only camera.)
 *
 * WHY THIS IS A SEPARATE FILE FROM camera.c
 * -----------------------------------------
 * Everything here is pure arithmetic on 3x3 matrices.  It needs no windows.h,
 * no MinHook, no OpenVR and no game, so it can be compiled into a console
 * program and EXECUTED -- which is the only reason to believe any of it.  The
 * camera hook links the same object file that the offline check runs against;
 * there is no second copy of the maths to drift.
 *
 *      out/headtrack_mathcheck.exe   `make check`, runs under wine, imports
 *                                    KERNEL32 + msvcrt and nothing else.
 *
 * CONVENTIONS (both MEASURED elsewhere, restated here so this file is readable
 * on its own)
 * --------------------------------------------------------------------------
 * A "basis" in this file is 9 floats, row-major, ROWS = (forward, LEFT, up),
 * each row a unit vector expressed in the coordinates of some parent frame.
 * That is exactly refdef+0x34's layout (camera-hook-plan §2.1, confirmed live
 * in exp 13) and exactly poses_pose_t.cod_axis's layout (exp 12 §3.1).  The
 * system is right-handed: forward x left = up, det = +1.
 *
 * Because the rows are the child frame's axes written in parent coordinates, a
 * vector `a` given in CHILD coordinates has PARENT coordinates
 *
 *      parent = a[0]*row0 + a[1]*row1 + a[2]*row2      ( = M^T a )
 *
 * and that single fact fixes every multiplication order below.  See
 * ht_compose().
 *
 * THE COMPOSITION, IN ONE LINE
 * ----------------------------
 *      F = H * G
 * where
 *      G = the REFERENCE basis: where the game says the player's body faces,
 *          rows in WORLD coordinates.  Built from refdef.viewaxis by
 *          ht_build_reference().
 *      H = the HEAD basis: where the head faces, rows in TRACKING-SPACE
 *          coordinates, already converted to the CoD convention by exp 12's
 *          poses_cod_from_ovr_m34() -- we do not re-derive that transform, we
 *          reuse it.
 *      F = where the head faces, rows in WORLD coordinates.  This is what gets
 *          written into refdef.viewaxis.
 *
 * H = identity (head aligned with tracking-space forward) must give F = G, i.e.
 * the game's own view, untouched.  That is case 1 of the self-check.
 *
 * TWO REFERENCE MODES, AND WHY THE DEFAULT IS YAW-ONLY
 * ---------------------------------------------------
 * HT_REF_YAW_ONLY  G is a pure yaw about world up, taken from the game view's
 *                  heading.  The game's PITCH and ROLL are discarded and come
 *                  from the head instead.  This keeps the horizon level: with a
 *                  full-orientation reference, a mouse-pitched body frame makes
 *                  a head yaw rotate about a TILTED axis, and the world rolls
 *                  when you turn your head.  Default.
 * HT_REF_FULL      G is the game's view axis verbatim.  Mouse pitch still
 *                  works and head pitch stacks on top of it.  Kept because it
 *                  is one line and because "the horizon rolls" and "pitch does
 *                  nothing" are the two complaints this choice trades between,
 *                  and only a headset can settle which is worse.
 *
 * The yaw-only mode buys one more thing: an exact runtime invariant.  A pure
 * yaw about up leaves the third COLUMN of a basis alone, so
 *
 *      F[i][2] == H[i][2]  for i = 0,1,2, EXACTLY (to float rounding)
 *
 * i.e. the up-component of the final forward/left/up rows must equal the
 * up-component of the head's.  ht_check_yaw_invariant() tests it on live data
 * every frame, and needs no knowledge of what the correct answer is.
 *
 * BE PRECISE ABOUT WHAT IT WATCHES.  An earlier version of this comment claimed
 * it caught "any sign error in the yaw matrix".  It does not, and the claim was
 * measured false during review:
 *
 *      WHAT IT CATCHES   ht_compose failing to carry H's third column through
 *                        unchanged -- so a TRANSPOSED H, a SWAPPED order
 *                        (G*H), and arithmetic damage inside ht_compose.
 *                        Case 6 verifies each of those is rejected.
 *      WHAT IT CANNOT    ANY error in G.  The yaw-only branch of
 *                        ht_build_reference writes G's third column as the
 *                        literal (0,0,1), so every wrong G that shares that
 *                        column satisfies the invariant identically: H*G^T,
 *                        H*yaw(wrong angle) and even H*I were all measured at
 *                        err = 0.000000 and ACCEPTED.
 *
 * G is pinned by cases 1 and 9 instead, offline.  The invariant is a watchdog
 * on one multiplication, not on the whole pipeline, and calling it more than
 * that would be exactly the kind of reassuring instrument this project keeps
 * being burned by.
 */
#ifndef BO1VR_HEADTRACK_H
#define BO1VR_HEADTRACK_H

enum {
    HT_REF_YAW_ONLY = 0,   /* default: horizon stays level */
    HT_REF_FULL     = 1    /* game orientation verbatim    */
};

/* Build the reference basis G (rows in world coords) from a game view axis.
 *
 * Returns 1 on success, 0 if the game axis is degenerate in a way this cannot
 * recover from (not orthonormal, or both the forward and left rows vertical) --
 * in which case *out_G is untouched and the caller MUST NOT write a view.
 *
 * In HT_REF_YAW_ONLY the heading is taken from the forward row; if the player
 * is looking near-vertically that row has almost no horizontal part, so the
 * heading comes from the LEFT row instead, which is horizontal exactly when
 * forward is not. */
int ht_build_reference(const float game_axis[9], int mode, float out_G[9]);

/* out = yaw matrix, rows (cos,sin,0) / (-sin,cos,0) / (0,0,1).  As an operator
 * on column vectors this rotates by -rad about world up; as a basis its rows
 * are the axes of a frame yawed by +rad (i.e. turned to its LEFT, because CoD
 * +Y is left). */
void ht_yaw_matrix(float rad, float out[9]);

/* out_F = H * G.  Aliasing is safe. */
void ht_compose(const float H[9], const float G[9], float out_F[9]);

/* Yaw of a basis's forward row, radians, atan2(row0.y, row0.x).  0 when the
 * forward row is vertical. */
float ht_yaw_of(const float M[9]);

/* Rotate every ROW of M BY +rad about world up (each row through ht_vec_yaw).
 * This is the recentre operation: called with rad = -ht_yaw_of(H) it makes the
 * head's current heading read as "straight ahead", leaving pitch and roll
 * untouched.  Aliasing safe. */
void ht_rotate_rows_yaw(const float M[9], float rad, float out[9]);

/* Rotate one vector BY +rad about up: +X toward +Y, i.e. toward CoD left.
 * Same sense as ht_yaw_matrix's rows -- see the note in headtrack.c, this pair
 * is a transpose apart and swapping them is a real bug case 7 catches.
 * Aliasing safe. */
void ht_vec_yaw(const float v[3], float rad, float out[3]);

/* A vector given in G's frame -> the parent (world) frame: out = G^T v.
 * Aliasing safe.  This is how a tracking-space offset (the eye's position
 * relative to the head, or a lean) becomes a world-space one. */
void ht_ref_to_world(const float G[9], const float v_ref[3], float out_world[3]);

/* --- the checker -------------------------------------------------------- */

typedef struct ht_basis_s {
    float len[3];      /* row lengths, want 1                       */
    float dot[3];      /* row 0.1, 0.2, 1.2, want 0                 */
    float det;         /* want +1                                   */
    float cross_err;   /* |row0 x row1 - row2|, want 0              */
    int   mirrored;    /* det < 0: a LEFT-handed basis              */
    int   ok;
} ht_basis_t;

/* Is M a right-handed orthonormal basis?  Returns r->ok.
 *
 * NOTE the difference from exp 13's original check_axis(), which tested
 * fabsf(fabsf(det) - 1) and therefore PASSED a mirrored basis -- precisely the
 * failure mode a rotation bug produces.  This one requires det = +1 and reports
 * `mirrored` separately so the log names the fault instead of just failing. */
int ht_check_basis(const float M[9], ht_basis_t *r);

/* The runtime invariant described at the top: in yaw-only mode the third
 * column of F must equal the third column of H.  Returns 1 if it holds.
 * If `err` is non-NULL it receives the largest component difference.
 *
 * Watches ht_compose only.  Blind to every error in G -- see the header comment
 * for the measurements. */
int ht_check_yaw_invariant(const float F[9], const float H[9], float tol,
                           float *err);

/* Offline check of everything above, against hand-derived closed forms rather
 * than against this file's own matrix code.  Returns 0 on success or the number
 * of the first failing case, with an explanation in `why` (may be NULL).
 * Loads nothing, connects to nothing, allocates nothing. */
int ht_selfcheck(char *why, int cap);

/* Number of cases ht_selfcheck runs, so a caller can report "n/n". */
#define HT_SELFCHECK_CASES 10

#endif /* BO1VR_HEADTRACK_H */
