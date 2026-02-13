
---

## NBA Points Model (C)

Repo: `nba-points-model`

```markdown
# NBA Points Projection Model (C)

Projection system estimating NBA player scoring output using betting lines, season averages, pace, and defense vs position.

## Overview

This model blends market expectations with statistical indicators to generate scoring projections for NBA players.

## Inputs

- Sportsbook point lines
- Season scoring averages
- Pace metrics
- Defense vs position data

## Modeling Logic

Projection is computed via weighted blending of:
- Market baseline
- Season production
- Pace-adjusted scoring expectation
- Defensive matchup modifier

## Compile

```bash
gcc points_projection.c -o points_model
