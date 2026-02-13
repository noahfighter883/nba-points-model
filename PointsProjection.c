/* nba_points_model.c
 * A simple, configurable C model to project an NBA player's points.
 *
 * Primary drivers:
 *   - Sportsbook points line (PLAYER_LINE)
 *   - Player season points average (SEASON_AVG_PTS)
 *
 * Secondary adjustments (multiplicative):
 *   - Home vs Away
 *   - Game Total O/U (light)
 *   - Team Total O/U (moderate)
 *   - Defense vs Position (opp allows points vs this pos)
 * Optional knobs:
 *   - Recent form (last N games avg vs season avg)
 *   - Minutes trend (expected minutes vs season minutes)
 *   - Pace factor for matchup
 *   - Back-to-back penalty
 *
 * Everything is tunable in WEIGHTS & BASELINES.
 */

#include <stdio.h>

/*======================== TUNABLE WEIGHTS & CAPS ========================*/

/* Base blend between player line and season average (should sum to ~1.0) */
static const double W_BASE_LINE        = 0.60;
static const double W_BASE_SEASON_AVG  = 0.40;

/* Multipliers (all applied to the blended base) */
static const double W_HOME_AWAY        = 0.04;  /* +4% home, -4% away by default */
static const double W_GAME_TOTAL       = 0.06;  /* light: sensitivity to game O/U vs league baseline */
static const double W_TEAM_TOTAL       = 0.12;  /* moderate: team O/U vs league baseline */
static const double W_DEF_VS_POS       = 0.14;  /* opponent allows vs pos vs league baseline */

/* Optional extras — set their weights to 0.0 to disable */
static const double W_RECENT_FORM      = 0.08;  /* last-N avg vs season avg (relative) */
static const double W_MINUTES_TREND    = 0.10;  /* expected minutes vs season minutes (relative) */
static const double W_PACE             = 0.06;  /* matchup pace vs league average pace (relative) */
static const double W_B2B_PENALTY      = 0.03;  /* subtract up to 3% if on B2B */

/* Baselines (edit as you see fit) */
static const double LEAGUE_AVG_GAME_TOTAL      = 229.0;
static const double LEAGUE_AVG_TEAM_TOTAL      = 114.5;
static const double LEAGUE_AVG_PACE            = 99.5;  /* possessions per team per game approx */
static const double LEAGUE_BASE_PTS_ALLOWED_POS = 23.0; /* avg points allowed to this position */

/* Caps on how far multipliers can move (to avoid extreme outputs) */
static const double MULT_MIN = 0.70;
static const double MULT_MAX = 1.40;

/* Simple clamp helper */
static double clamp(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/*======================== INPUT STRUCTS ========================*/

typedef struct {
    /* Core */
    const char *player_name;
    double player_line_pts;        /* Sportsbook points line */
    double season_avg_pts;         /* Season average points */

    /* Context */
    int is_home;                   /* 1 = home, 0 = away */
    double game_total_ou;          /* Game O/U total points */
    double team_total_ou;          /* Team O/U points */

    /* Defense vs position: opponent points allowed per game to player's position */
    double opp_pts_allowed_vs_pos; /* numeric rate; compare to LEAGUE_BASE_PTS_ALLOWED_POS */

    /* Optional extras */
    double recent_avg_pts;         /* last N games avg; set = season_avg_pts if unused */
    double season_avg_minutes;     /* season avg minutes */
    double expected_minutes;       /* expected minutes for this game */
    double matchup_pace;           /* projected pace for game (possessions per team) */
    int is_back_to_back;           /* 1 if on B2B, else 0 */
} Inputs;

typedef struct {
    double base_points;
    double mult_homeaway;
    double mult_game_total;
    double mult_team_total;
    double mult_def_pos;
    double mult_recent;
    double mult_minutes;
    double mult_pace;
    double mult_b2b;

    double uncapped_multiplier;
    double final_multiplier;
    double projection;
} Output;

/*======================== MODEL FUNCTIONS ========================*/

static double base_points(const Inputs *in) {
    return W_BASE_LINE * in->player_line_pts
         + W_BASE_SEASON_AVG * in->season_avg_pts;
}

static double homeaway_multiplier(const Inputs *in) {
    /* Simple: +W_HOME_AWAY at home, -W_HOME_AWAY away */
    double delta = in->is_home ? +W_HOME_AWAY : -W_HOME_AWAY;
    return 1.0 + delta;
}

static double game_total_multiplier(const Inputs *in) {
    /* Normalize by league avg and weight: (OU - baseline)/baseline scaled by W_GAME_TOTAL */
    double rel = (in->game_total_ou - LEAGUE_AVG_GAME_TOTAL) / LEAGUE_AVG_GAME_TOTAL;
    return 1.0 + rel * W_GAME_TOTAL;
}

static double team_total_multiplier(const Inputs *in) {
    double rel = (in->team_total_ou - LEAGUE_AVG_TEAM_TOTAL) / LEAGUE_AVG_TEAM_TOTAL;
    return 1.0 + rel * W_TEAM_TOTAL;
}

static double defense_vs_pos_multiplier(const Inputs *in) {
    /* If opp allows more than baseline to this position -> boost; less -> penalty */
    double rel = 0.0;
    if (LEAGUE_BASE_PTS_ALLOWED_POS > 0.0) {
        rel = (in->opp_pts_allowed_vs_pos - LEAGUE_BASE_PTS_ALLOWED_POS)
              / LEAGUE_BASE_PTS_ALLOWED_POS;
    }
    return 1.0 + rel * W_DEF_VS_POS;
}

static double recent_form_multiplier(const Inputs *in) {
    if (W_RECENT_FORM == 0.0 || in->season_avg_pts <= 0.0) return 1.0;
    double rel = (in->recent_avg_pts - in->season_avg_pts) / in->season_avg_pts;
    return 1.0 + rel * W_RECENT_FORM;
}

static double minutes_trend_multiplier(const Inputs *in) {
    if (W_MINUTES_TREND == 0.0 || in->season_avg_minutes <= 0.0) return 1.0;
    double rel = (in->expected_minutes - in->season_avg_minutes) / in->season_avg_minutes;
    return 1.0 + rel * W_MINUTES_TREND;
}

static double pace_multiplier(const Inputs *in) {
    if (W_PACE == 0.0 || LEAGUE_AVG_PACE <= 0.0) return 1.0;
    double rel = (in->matchup_pace - LEAGUE_AVG_PACE) / LEAGUE_AVG_PACE;
    return 1.0 + rel * W_PACE;
}

static double b2b_multiplier(const Inputs *in) {
    if (!in->is_back_to_back || W_B2B_PENALTY <= 0.0) return 1.0;
    /* Simple fixed penalty when on a back-to-back */
    return 1.0 - W_B2B_PENALTY;
}

static Output project(const Inputs *in) {
    Output out;

    out.base_points     = base_points(in);
    out.mult_homeaway   = homeaway_multiplier(in);
    out.mult_game_total = game_total_multiplier(in);
    out.mult_team_total = team_total_multiplier(in);
    out.mult_def_pos    = defense_vs_pos_multiplier(in);
    out.mult_recent     = recent_form_multiplier(in);
    out.mult_minutes    = minutes_trend_multiplier(in);
    out.mult_pace       = pace_multiplier(in);
    out.mult_b2b        = b2b_multiplier(in);

    out.uncapped_multiplier =
        out.mult_homeaway *
        out.mult_game_total *
        out.mult_team_total *
        out.mult_def_pos *
        out.mult_recent *
        out.mult_minutes *
        out.mult_pace *
        out.mult_b2b;

    out.final_multiplier = clamp(out.uncapped_multiplier, MULT_MIN, MULT_MAX);
    out.projection = out.base_points * out.final_multiplier;
    return out;
}

/*======================== DEMO / INTERACTIVE ========================*/

static void print_output(const Inputs *in, const Output *o) {
    printf("\nProjection for %s\n", in->player_name);
    printf("Base points (blend): %.2f\n", o->base_points);
    printf("Multipliers:\n");
    printf("  Home/Away         : %.4f\n", o->mult_homeaway);
    printf("  Game Total (OU)   : %.4f\n", o->mult_game_total);
    printf("  Team Total (OU)   : %.4f\n", o->mult_team_total);
    printf("  Def vs Position   : %.4f\n", o->mult_def_pos);
    printf("  Recent Form       : %.4f\n", o->mult_recent);
    printf("  Minutes Trend     : %.4f\n", o->mult_minutes);
    printf("  Pace              : %.4f\n", o->mult_pace);
    printf("  Back-to-Back      : %.4f\n", o->mult_b2b);
    printf("Uncapped Multiplier : %.4f\n", o->uncapped_multiplier);
    printf("Final Multiplier    : %.4f  (capped to [%.2f, %.2f])\n", o->final_multiplier, MULT_MIN, MULT_MAX);
    printf("Projected Points    : %.2f\n\n", o->projection);
}

int main(void) {
    Inputs in;

    /* === Prompt user for inputs from terminal === */
    char namebuf[128];
    printf("Player name: ");
    if (!fgets(namebuf, sizeof(namebuf), stdin)) return 0;
    /* strip newline */
    for (int i = 0; namebuf[i]; ++i) { if (namebuf[i] == '\n') { namebuf[i] = 0; break; } }
    in.player_name = namebuf;

    printf("Sportsbook line (points): ");
    scanf("%lf", &in.player_line_pts);

    printf("Season avg points: ");
    scanf("%lf", &in.season_avg_pts);

    printf("Is home? (1=yes, 0=no): ");
    scanf("%d", &in.is_home);

    printf("Game total O/U: ");
    scanf("%lf", &in.game_total_ou);

    printf("Team total O/U: ");
    scanf("%lf", &in.team_total_ou);

    printf("Opponent points allowed to this position (per game): ");
    scanf("%lf", &in.opp_pts_allowed_vs_pos);

    /* Optional extras (enter season values again to neutralize effects) */
    printf("Recent avg points (last N; enter season avg to ignore): ");
    scanf("%lf", &in.recent_avg_pts);

    printf("Season avg minutes: ");
    scanf("%lf", &in.season_avg_minutes);

    printf("Expected minutes this game: ");
    scanf("%lf", &in.expected_minutes);

    printf("Matchup pace (possessions per team): ");
    scanf("%lf", &in.matchup_pace);

    printf("Back-to-back? (1=yes, 0=no): ");
    scanf("%d", &in.is_back_to_back);

    /* Compute & print */
    Output out = project(&in);
    print_output(&in, &out);

    /* Tip: tweak the weights/constants at the top to calibrate your model
       to historical data or to your personal handicapping philosophy. */

    return 0;
}
