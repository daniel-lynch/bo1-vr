/* headtrack.c -- the HMD-orientation maths for the camera hook, and the
 * offline check that decides whether to believe it.  See headtrack.h for the
 * conventions and for why F = H * G and not one of the seven other orderings.
 *
 * Three builds from this one file:
 *   (nothing)             out/headtrack.o        linked into camera.asi
 *   -DHEADTRACK_MATHCHECK out/headtrack_mathcheck.exe   `make check`
 *
 * NO windows.h, no OpenVR, no allocation, no global state.  Every function is
 * a pure function of its arguments, which is what makes the offline check a
 * check of the shipping code rather than of a copy of it.
 */

#include <math.h>
#include <string.h>

#include "headtrack.h"

/* --------------------------------------------------------------- helpers */

static float dot3(const float a[3], const float b[3])
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void cross3(const float a[3], const float b[3], float out[3])
{
    float t[3];
    t[0] = a[1]*b[2] - a[2]*b[1];
    t[1] = a[2]*b[0] - a[0]*b[2];
    t[2] = a[0]*b[1] - a[1]*b[0];
    memcpy(out, t, sizeof(t));
}

static float det3(const float m[9])
{
    return m[0]*(m[4]*m[8] - m[5]*m[7])
         - m[1]*(m[3]*m[8] - m[5]*m[6])
         + m[2]*(m[3]*m[7] - m[4]*m[6]);
}

/* ------------------------------------------------------------- the maths */

void ht_yaw_matrix(float rad, float out[9])
{
    float c = cosf(rad), s = sinf(rad);
    out[0] =  c; out[1] = s; out[2] = 0.0f;   /* forward */
    out[3] = -s; out[4] = c; out[5] = 0.0f;   /* left    */
    out[6] = 0.0f; out[7] = 0.0f; out[8] = 1.0f;  /* up    */
}

void ht_compose(const float H[9], const float G[9], float out_F[9])
{
    float t[9];
    int i, j;
    /* t[i][j] = sum_k H[i][k] * G[k][j].
     *
     * Row i of the result is  H[i][0]*Grow0 + H[i][1]*Grow1 + H[i][2]*Grow2,
     * i.e. the head's i-th axis -- whose components in the REFERENCE frame are
     * H's row i -- re-expressed in the reference frame's parent, which is the
     * world.  That is the derivation in headtrack.h, written out. */
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            t[i*3 + j] = H[i*3 + 0] * G[0*3 + j]
                       + H[i*3 + 1] * G[1*3 + j]
                       + H[i*3 + 2] * G[2*3 + j];
    memcpy(out_F, t, sizeof(t));
}

float ht_yaw_of(const float M[9])
{
    if (M[0]*M[0] + M[1]*M[1] < 1e-12f)
        return 0.0f;
    return atan2f(M[1], M[0]);
}

/* Rotate v BY +rad about world up -- +X toward +Y, i.e. toward CoD left.  This
 * is the same sense as ht_yaw_matrix(): ht_yaw_matrix(r)'s rows are the
 * identity's rows put through ht_vec_yaw(.., r), which is the property case 7
 * depends on.  (As a matrix this is ht_yaw_matrix(r) TRANSPOSED, because that
 * matrix's rows are a basis, and a basis maps parent -> child while rotating a
 * vector maps child -> parent.  Getting these two the same way round is
 * exactly the kind of slip case 7 exists to catch.) */
void ht_vec_yaw(const float v[3], float rad, float out[3])
{
    float c = cosf(rad), s = sinf(rad);
    float x = v[0], y = v[1];
    out[0] = c*x - s*y;
    out[1] = s*x + c*y;
    out[2] = v[2];
}

void ht_rotate_rows_yaw(const float M[9], float rad, float out[9])
{
    int i;
    for (i = 0; i < 3; i++)
        ht_vec_yaw(M + i*3, rad, out + i*3);
}

void ht_ref_to_world(const float G[9], const float v_ref[3], float out_world[3])
{
    float t[3];
    int j;
    for (j = 0; j < 3; j++)
        t[j] = v_ref[0]*G[0*3 + j] + v_ref[1]*G[1*3 + j] + v_ref[2]*G[2*3 + j];
    memcpy(out_world, t, sizeof(t));
}

int ht_check_basis(const float M[9], ht_basis_t *r)
{
    ht_basis_t tmp;
    float c[3], d[3];
    int i;

    if (!r) r = &tmp;
    memset(r, 0, sizeof(*r));
    if (!M) return 0;

    for (i = 0; i < 3; i++) {
        float l2 = dot3(M + i*3, M + i*3);
        /* No isnan(): a NaN fails every comparison below anyway, which is the
         * behaviour we want and does not need a libm predicate to get. */
        r->len[i] = sqrtf(l2);
    }
    r->dot[0] = dot3(M + 0, M + 3);
    r->dot[1] = dot3(M + 0, M + 6);
    r->dot[2] = dot3(M + 3, M + 6);
    r->det    = det3(M);

    cross3(M + 0, M + 3, c);
    d[0] = c[0] - M[6]; d[1] = c[1] - M[7]; d[2] = c[2] - M[8];
    r->cross_err = sqrtf(dot3(d, d));

    r->mirrored = (r->det < 0.0f);

    r->ok = fabsf(r->len[0] - 1.0f) < 1e-3f
         && fabsf(r->len[1] - 1.0f) < 1e-3f
         && fabsf(r->len[2] - 1.0f) < 1e-3f
         && fabsf(r->dot[0]) < 1e-3f
         && fabsf(r->dot[1]) < 1e-3f
         && fabsf(r->dot[2]) < 1e-3f
         /* det = +1, NOT |det| = 1.  A mirrored basis is orthonormal and has
          * |det| = 1; it is exactly what a sign error produces, and exactly
          * what must not reach the renderer. */
         && (r->det > 0.999f && r->det < 1.001f)
         && r->cross_err < 1e-3f;
    return r->ok;
}

int ht_check_yaw_invariant(const float F[9], const float H[9], float tol,
                           float *err)
{
    float e = 0.0f;
    int i;
    for (i = 0; i < 3; i++) {
        float d = fabsf(F[i*3 + 2] - H[i*3 + 2]);
        if (d > e) e = d;
    }
    if (err) *err = e;
    return e <= tol;
}

int ht_build_reference(const float game_axis[9], int mode, float out_G[9])
{
    float hx, hy, n2, inv, c, s;

    if (!game_axis || !out_G)
        return 0;

    /* Refuse to build anything from a view axis that is not a view axis. If
     * the refdef ever hands us garbage -- a torn read, a menu frame, a wrong
     * offset after a game patch -- the right answer is to leave the camera
     * alone, not to write a "best effort" basis into it. */
    if (!ht_check_basis(game_axis, 0))
        return 0;

    if (mode == HT_REF_FULL) {
        memcpy(out_G, game_axis, 9 * sizeof(float));
        return 1;
    }

    /* Heading from the forward row's horizontal part. */
    hx = game_axis[0];
    hy = game_axis[1];
    n2 = hx*hx + hy*hy;
    if (n2 < 0.01f) {
        /* Looking near-vertically: forward has almost no heading left in it.
         * The LEFT row is horizontal exactly when forward is vertical, and it
         * carries the same heading: left = (-sin, cos, 0). */
        hx =  game_axis[4];
        hy = -game_axis[3];
        n2 = hx*hx + hy*hy;
        if (n2 < 0.01f)
            return 0;          /* both vertical: impossible for a real basis */
    }
    inv = 1.0f / sqrtf(n2);
    c = hx * inv;
    s = hy * inv;

    out_G[0] =  c; out_G[1] = s; out_G[2] = 0.0f;
    out_G[3] = -s; out_G[4] = c; out_G[5] = 0.0f;
    out_G[6] = 0.0f; out_G[7] = 0.0f; out_G[8] = 1.0f;
    return 1;
}

/* ====================================================== THE SELF-CHECK ====
 *
 * A rotation bug does not announce itself.  Position errors are visible the
 * instant you look at the screen; an orientation basis that is transposed, or
 * composed in the wrong order, or mirrored in one axis, produces a world that
 * still moves smoothly with your head and still looks like a world.  exp 13's
 * RESULTS.md §"Deliberately position-only" is a note-to-self that this project
 * already lost time to exactly that class of bug.
 *
 * So the check below is written to FAIL on the specific mutations, not to
 * confirm that some floats came out finite:
 *
 *   * every expected value is a HAND-DERIVED CLOSED FORM in sines and cosines.
 *     It is never computed by calling the function under test, so an error in
 *     ht_compose cannot appear on both sides of the comparison.
 *   * case 3 additionally computes the four WRONG answers -- G*H, H^T*G, H*G^T
 *     and (H*G)^T -- and asserts that each is far from the right one.  That
 *     makes "this test can tell a transpose from the truth" a tested property
 *     of the test rather than a claim in a comment.
 *   * case 5 feeds ht_check_basis a mirrored basis and requires it to be
 *     REJECTED, because a checker that accepts everything is worse than none.
 *   * case 6 does the same for the runtime yaw invariant: it must reject a
 *     deliberately transposed composition.
 */

#define HT_PI 3.14159265358979323846f
#define DEG(x) ((x) * (HT_PI / 180.0f))

static int ht_close(float a, float b, float eps) { return fabsf(a - b) < eps; }

static int ht_close3(const float a[3], const float b[3], float eps)
{
    return ht_close(a[0], b[0], eps) && ht_close(a[1], b[1], eps)
        && ht_close(a[2], b[2], eps);
}

static int ht_close9(const float a[9], const float b[9], float eps)
{
    int i;
    for (i = 0; i < 9; i++)
        if (!ht_close(a[i], b[i], eps))
            return 0;
    return 1;
}

/* Largest absolute difference between two 3x3s -- used to prove that a wrong
 * ordering is not merely different, but grossly different. */
static float ht_maxdiff9(const float a[9], const float b[9])
{
    float m = 0.0f;
    int i;
    for (i = 0; i < 9; i++) {
        float d = fabsf(a[i] - b[i]);
        if (d > m) m = d;
    }
    return m;
}

/* A CoD-convention basis for a view yawed by `y` and pitched up by `p`.  This
 * is the closed form the expectations are built from; it is deliberately NOT
 * expressed in terms of ht_compose or ht_yaw_matrix. */
static void ht_euler(float y, float p, float out[9])
{
    float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
    out[0] =  cp*cy; out[1] =  cp*sy; out[2] =  sp;   /* forward */
    out[3] = -sy;    out[4] =  cy;    out[5] =  0.0f; /* left    */
    out[6] = -sp*cy; out[7] = -sp*sy; out[8] =  cp;   /* up      */
}

static void ht_copy_why(char *why, int cap, const char *msg)
{
    int i = 0;
    if (!why || cap <= 0) return;
    while (msg[i] && i < cap - 1) { why[i] = msg[i]; i++; }
    why[i] = '\0';
}

int ht_selfcheck(char *why, int cap)
{
    const float EPS = 2e-4f;
    float G[9], H[9], F[9], E[9], T[9], W[9];
    float v[3], w[3], e3[3];
    ht_basis_t rep;

    ht_copy_why(why, cap, "");

    /* --- 1.  Identity head leaves the game's view alone. --------------- */
    {
        float game[9];
        ht_euler(DEG(30), DEG(20), game);          /* game is yawed AND pitched */
        if (!ht_build_reference(game, HT_REF_YAW_ONLY, G)) {
            ht_copy_why(why, cap, "case 1: ht_build_reference rejected a valid view axis");
            return 1;
        }
        ht_yaw_matrix(DEG(30), E);                 /* yaw-only ref must be exactly this */
        if (!ht_close9(G, E, EPS)) {
            ht_copy_why(why, cap, "case 1: yaw-only reference did not reproduce the game heading");
            return 1;
        }
        memset(H, 0, sizeof H); H[0] = H[4] = H[8] = 1.0f;
        ht_compose(H, G, F);
        if (!ht_close9(F, G, EPS)) {
            ht_copy_why(why, cap, "case 1: identity head changed the view (F != G)");
            return 1;
        }
    }

    /* --- 2.  Head yaw adds to game yaw, and in the LEFT direction. -----
     * A transposed head basis is the inverse rotation, so it would turn 30+40
     * into 30-40.  Only the sign of the sum distinguishes them, which is why
     * the two angles differ. */
    {
        ht_yaw_matrix(DEG(30), G);
        ht_yaw_matrix(DEG(40), H);
        ht_compose(H, G, F);
        ht_yaw_matrix(DEG(70), E);
        if (!ht_close9(F, E, EPS)) {
            ht_copy_why(why, cap, "case 2: head yaw did not add to game yaw (wrong direction or transposed)");
            return 2;
        }
        /* And it is genuinely 70, not 10: the wrong sign must be far away. */
        ht_yaw_matrix(DEG(-10), T);
        if (ht_maxdiff9(F, T) < 0.1f) {
            ht_copy_why(why, cap, "case 2: +70 and -10 are indistinguishable -- test is not discriminating");
            return 2;
        }
    }

    /* --- 3.  THE DISCRIMINATOR.  Yaw and pitch do not commute. ---------
     * Game yawed 90 degrees, head pitched 40 degrees up and not yawed.  The
     * head pitches about the REFERENCE frame's left axis, so:
     *      forward = (cos p cos y, cos p sin y, sin p)
     *      left    = (-sin y, cos y, 0)                 (unchanged by pitch)
     *      up      = (-sin p cos y, -sin p sin y, cos p)
     * Every wrong ordering below gives a different answer, and the case proves
     * it rather than asserting it. */
    {
        const float y = DEG(90), p = DEG(40);
        float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
        float game[9], HT[9], GT[9], FT[9];
        int i, j;

        ht_euler(y, DEG(25), game);   /* the game's own pitch is DISCARDED */
        if (!ht_build_reference(game, HT_REF_YAW_ONLY, G)) {
            ht_copy_why(why, cap, "case 3: ht_build_reference rejected a valid view axis");
            return 3;
        }
        ht_euler(0.0f, p, H);         /* head: pure pitch, no yaw */
        ht_compose(H, G, F);

        E[0] =  cp*cy; E[1] =  cp*sy; E[2] =  sp;
        E[3] = -sy;    E[4] =  cy;    E[5] =  0.0f;
        E[6] = -sp*cy; E[7] = -sp*sy; E[8] =  cp;
        if (!ht_close9(F, E, EPS)) {
            ht_copy_why(why, cap, "case 3: composition is wrong (order, transpose or sign)");
            return 3;
        }

        /* The four wrong answers, each of which a plausible slip produces.
         * Each must be GROSSLY different from the right one -- not merely
         * different, or the case would pass on a lucky pair of angles. */
        {
            int blunt = 0;
            for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) {
                HT[i*3+j] = H[j*3+i];
                GT[i*3+j] = G[j*3+i];
            }
            ht_compose(G, H, T);   /* swapped order      */
            if (ht_maxdiff9(T, E) < 0.1f) blunt = 1;
            ht_compose(HT, G, T);  /* head transposed    */
            if (ht_maxdiff9(T, E) < 0.1f) blunt = 1;
            ht_compose(H, GT, T);  /* reference transposed */
            if (ht_maxdiff9(T, E) < 0.1f) blunt = 1;
            for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) FT[i*3+j] = F[j*3+i];
            if (ht_maxdiff9(FT, E) < 0.1f) blunt = 1;   /* result transposed */
            if (blunt) {
                ht_copy_why(why, cap,
                    "case 3: a WRONG ordering scores as correct -- the test cannot see a transpose");
                return 3;
            }
        }
    }

    /* --- 4.  Handedness survives a compound head rotation. -------------
     * Mirroring is the failure this whole exercise exists to catch: a mirrored
     * basis is still orthonormal, still smooth, still moves with your head. */
    {
        float roll[9], tmp[9];
        ht_euler(DEG(-55), DEG(-15), H);
        /* roll 25 degrees about forward: left and up rotate, forward does not */
        {
            float cr = cosf(DEG(25)), sr = sinf(DEG(25));
            roll[0] = 1; roll[1] = 0;   roll[2] = 0;
            roll[3] = 0; roll[4] = cr;  roll[5] = sr;
            roll[6] = 0; roll[7] = -sr; roll[8] = cr;
        }
        ht_compose(roll, H, tmp);
        ht_yaw_matrix(DEG(115), G);
        ht_compose(tmp, G, F);
        if (!ht_check_basis(F, &rep)) {
            ht_copy_why(why, cap, rep.mirrored
                ? "case 4: composed basis is MIRRORED (det < 0)"
                : "case 4: composed basis is not orthonormal");
            return 4;
        }
        if (!ht_close(rep.det, 1.0f, 1e-3f)) {
            ht_copy_why(why, cap, "case 4: det != +1");
            return 4;
        }
    }

    /* --- 5.  The checker rejects what it must. ------------------------- */
    {
        float M[9];
        memset(M, 0, sizeof M); M[0] = M[4] = M[8] = 1.0f;
        if (!ht_check_basis(M, &rep)) {
            ht_copy_why(why, cap, "case 5: identity rejected");
            return 5;
        }
        M[3] = 0.0f; M[4] = -1.0f;            /* negate the LEFT row: a mirror */
        if (ht_check_basis(M, &rep) || !rep.mirrored) {
            ht_copy_why(why, cap, "case 5: a MIRRORED basis passed ht_check_basis -- the checker is a rubber stamp");
            return 5;
        }
        memset(M, 0, sizeof M); M[0] = M[4] = M[8] = 2.0f;   /* scaled */
        if (ht_check_basis(M, &rep)) {
            ht_copy_why(why, cap, "case 5: a scaled basis passed ht_check_basis");
            return 5;
        }
        memset(M, 0, sizeof M); M[0] = M[4] = 1.0f; M[7] = 1.0f;  /* shear-ish */
        if (ht_check_basis(M, &rep)) {
            ht_copy_why(why, cap, "case 5: a non-orthonormal basis passed ht_check_basis");
            return 5;
        }
    }

    /* --- 6.  The runtime invariant, and proof that it bites. -----------
     * In yaw-only mode the reference rotation never touches world up, so the
     * third COLUMN of the composed basis must equal the third column of the
     * head basis, exactly.  The camera hook tests this every frame on live
     * data, where no closed form is available. */
    {
        float HT[9];
        int i, j;
        ht_euler(DEG(20), DEG(35), H);
        ht_yaw_matrix(DEG(-70), G);
        ht_compose(H, G, F);
        if (!ht_check_yaw_invariant(F, H, 1e-4f, 0)) {
            ht_copy_why(why, cap, "case 6: the yaw invariant fails on a CORRECT composition");
            return 6;
        }
        for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) HT[i*3+j] = H[j*3+i];
        ht_compose(HT, G, T);
        if (ht_check_yaw_invariant(T, H, 1e-4f, 0)) {
            ht_copy_why(why, cap, "case 6: the yaw invariant accepts a TRANSPOSED head basis -- it is not discriminating");
            return 6;
        }
        ht_compose(G, H, T);
        if (ht_check_yaw_invariant(T, H, 1e-4f, 0)) {
            ht_copy_why(why, cap, "case 6: the yaw invariant accepts a SWAPPED multiplication order");
            return 6;
        }
    }

    /* --- 7.  Recentre.  Head yawed by psi, recentred by -psi, is straight
     * ahead: the composed view is the game's own view again. */
    {
        const float psi = DEG(35);
        float Hr[9];
        ht_yaw_matrix(DEG(140), G);
        ht_euler(psi, DEG(12), H);
        if (!ht_close(ht_yaw_of(H), psi, 1e-3f)) {
            ht_copy_why(why, cap, "case 7: ht_yaw_of did not recover the head's yaw");
            return 7;
        }
        ht_rotate_rows_yaw(H, -ht_yaw_of(H), Hr);
        if (!ht_close(ht_yaw_of(Hr), 0.0f, 1e-3f)) {
            ht_copy_why(why, cap, "case 7: recentring did not zero the head yaw");
            return 7;
        }
        /* Pitch must survive the recentre untouched. */
        ht_euler(0.0f, DEG(12), E);
        if (!ht_close9(Hr, E, EPS)) {
            ht_copy_why(why, cap, "case 7: recentring disturbed the head's pitch");
            return 7;
        }
        ht_compose(Hr, G, F);
        ht_euler(DEG(140), DEG(12), E);
        if (!ht_close9(F, E, EPS)) {
            ht_copy_why(why, cap, "case 7: recentred head does not compose back onto the game view");
            return 7;
        }
    }

    /* --- 8.  Reference-frame vector -> world.  This is what turns the
     * eye-to-head offset (and any lean) into a world-space translation, so a
     * transpose here mirrors the stereo pair -- swapping the eyes -- while
     * leaving the picture otherwise perfect. */
    {
        const float y = DEG(50);
        ht_yaw_matrix(y, G);
        v[0] = 0.0f; v[1] = 1.0f; v[2] = 0.0f;    /* one unit to the LEFT */
        ht_ref_to_world(G, v, w);
        e3[0] = -sinf(y); e3[1] = cosf(y); e3[2] = 0.0f;
        if (!ht_close3(w, e3, EPS)) {
            ht_copy_why(why, cap, "case 8: ref->world is transposed (the stereo pair would be swapped)");
            return 8;
        }
        v[0] = 1.0f; v[1] = 0.0f; v[2] = 0.0f;    /* one unit FORWARD */
        ht_ref_to_world(G, v, w);
        e3[0] = cosf(y); e3[1] = sinf(y); e3[2] = 0.0f;
        if (!ht_close3(w, e3, EPS)) {
            ht_copy_why(why, cap, "case 8: ref->world forward is wrong");
            return 8;
        }
        /* And the inverse direction must NOT also pass: G^T v != G v here. */
        {
            float gv[3];
            gv[0] = G[0]*1.0f + G[1]*0.0f + G[2]*0.0f;
            gv[1] = G[3]*1.0f + G[4]*0.0f + G[5]*0.0f;
            gv[2] = G[6]*1.0f + G[7]*0.0f + G[8]*0.0f;
            if (ht_close3(gv, e3, 0.1f)) {
                ht_copy_why(why, cap, "case 8: G and G^T agree -- the case cannot see a transpose");
                return 8;
            }
        }
    }

    /* --- 9.  Degenerate view: straight up.  The heading has to come from
     * the left row, and the result must still be a legal basis. */
    {
        float game[9];
        ht_euler(DEG(0), DEG(89.9f), game);
        if (!ht_build_reference(game, HT_REF_YAW_ONLY, G)) {
            ht_copy_why(why, cap, "case 9: near-vertical view produced no reference");
            return 9;
        }
        if (!ht_check_basis(G, &rep)) {
            ht_copy_why(why, cap, "case 9: reference from a near-vertical view is not a basis");
            return 9;
        }
        ht_yaw_matrix(DEG(0), E);
        if (!ht_close9(G, E, 1e-2f)) {
            ht_copy_why(why, cap, "case 9: fallback heading disagrees with the forward row's");
            return 9;
        }
        /* Exactly vertical: forward is (0,0,1), heading must come from left. */
        game[0] = 0; game[1] = 0; game[2] = 1;
        game[3] = 0; game[4] = 1; game[5] = 0;
        game[6] = -1; game[7] = 0; game[8] = 0;
        if (!ht_build_reference(game, HT_REF_YAW_ONLY, G)) {
            ht_copy_why(why, cap, "case 9: exactly-vertical view produced no reference");
            return 9;
        }
        if (!ht_close9(G, E, 1e-3f)) {
            ht_copy_why(why, cap, "case 9: exactly-vertical fallback gave the wrong heading");
            return 9;
        }
    }

    /* --- 10.  Full mode passes the game orientation through, and a garbage
     * view axis is REFUSED rather than repaired. */
    {
        float game[9], bad[9];
        ht_euler(DEG(-25), DEG(18), game);
        if (!ht_build_reference(game, HT_REF_FULL, G) || !ht_close9(G, game, 1e-6f)) {
            ht_copy_why(why, cap, "case 10: full mode did not pass the game axis through");
            return 10;
        }
        memset(H, 0, sizeof H); H[0] = H[4] = H[8] = 1.0f;
        ht_compose(H, G, F);
        if (!ht_close9(F, game, EPS)) {
            ht_copy_why(why, cap, "case 10: identity head over a full reference changed the view");
            return 10;
        }
        memset(bad, 0, sizeof bad);          /* all zeros: not a basis */
        if (ht_build_reference(bad, HT_REF_YAW_ONLY, W) ||
            ht_build_reference(bad, HT_REF_FULL, W)) {
            ht_copy_why(why, cap, "case 10: a garbage view axis was ACCEPTED");
            return 10;
        }
        ht_euler(DEG(10), DEG(10), bad);
        bad[3] = -bad[3]; bad[4] = -bad[4];  /* mirrored: orthonormal, det -1 */
        if (ht_build_reference(bad, HT_REF_YAW_ONLY, W)) {
            ht_copy_why(why, cap, "case 10: a MIRRORED view axis was accepted as a reference");
            return 10;
        }
    }

    ht_copy_why(why, cap, "all cases pass");
    return 0;
}

/* ---------------------------------------------------------------- offline */

#ifdef HEADTRACK_MATHCHECK
#include <stdio.h>

int main(void)
{
    char why[192];
    int rc = ht_selfcheck(why, (int)sizeof why);
    if (rc == 0) {
        printf("headtrack_mathcheck: PASS (%d cases; no OpenVR, no game, no window)\n",
               HT_SELFCHECK_CASES);
        return 0;
    }
    printf("headtrack_mathcheck: FAIL at case %d: %s\n", rc, why);
    return 1;
}
#endif
