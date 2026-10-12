"""
Independent Python implementation of the ASHRAE/Viitanen mould-growth model.

This is a completely separate implementation from the device's C++ code (main_project.ino),
written in a different language, to independently verify that the algorithm was
implemented correctly on the device -- not by trusting the C++ code alone, but by
re-deriving the same published equations from scratch and checking both give the
same result on the same input data.

Sources (same equations used on the device):
- Hukka, A., & Viitanen, H. (1999). A mathematical model of mould growth on
  wooden material. Wood Science and Technology, 33(6), 475-485.
- Viitanen, H., & Ojanen, T. (2007). Improved Model to Predict Mold Growth in
  Building Materials. ASHRAE Buildings X Conference.
"""

import numpy as np
import pandas as pd
import os

# ===== Fixed material properties (the device assumes these, since it can't
# actually detect the wall material it's monitoring) =====
W = 0.0  # wood species: 0 = pine
SQ = 0.0  # surface quality: 0 = resawn (rough-cut) wood


def rh_crit(T):
    """
    Equation 1: the 'critical relative humidity' threshold for a given temperature.
    Below this humidity level, mould cannot grow at all at that temperature.
    """
    if T <= 20:
        return -0.00267 * T**3 + 0.160 * T**2 - 3.13 * T + 100.0
    else:
        return 80.0


def m_max(RH, rhc):
    """
    Equation 2: the maximum mould index that current humidity conditions can
    ever reach (growth slows down and plateaus as it approaches this ceiling).
    """
    if rhc == 100:
        return 6.0
    ratio = (rhc - RH) / (rhc - 100)
    return 1 + 7 * ratio - 2 * (ratio**2)


def t_m(T, RH):
    """Equation 3: time (in weeks) needed to reach mould index M=1 at this T/RH."""
    return np.exp(-0.68 * np.log(T) - 13.9 * np.log(RH) + 0.14 * W - 0.33 * SQ + 66.02)


def t_v(T, RH):
    """Equation 5: time (in weeks) needed to reach M=3, the point mould becomes visible."""
    return np.exp(-0.74 * np.log(T) - 12.72 * np.log(RH) + 0.06 * W + 61.50)


def k1_factor(M, T, RH):
    """Equation 6: a correction factor that slows growth once mould has already started."""
    if M < 1:
        return 1.0
    tm = t_m(T, RH)
    tv = t_v(T, RH)
    denom = (tv / tm) - 1
    if denom <= 0:
        return 1.0
    return 2.0 / denom


def k2_factor(M, Mmax):
    """Equation 7: a correction factor that slows growth as M approaches its ceiling (Mmax)."""
    return max(1 - np.exp(2.3 * (M - Mmax)), 0)


def decline_rate_per_hour(dry_hours):
    """
    Equation 9: once conditions are no longer favourable for growth, the mould
    index declines. The decline rate depends on how long it's been dry:
    - fast decline for the first 6 hours
    - no decline (index holds steady) between 6-24 hours
    - slow decline after 24 hours
    """
    # The published rates (-0.032 and -0.016) are per DAY, so divide by 24
    # to get the change in mould index per hour.
    if dry_hours <= 6:
        return -0.032 / 24.0
    elif dry_hours <= 24:
        return 0.0
    else:
        return -0.016 / 24.0


def mould_score_from_index(M):
    """
    Converts the mould index (0-6) into the 0-10 score shown on the device.
    The bands follow the descriptions of the mould index (Viitanen and Ojanen, 2007, Table 1):
    M < 1 -> 0-3 (SAFE); 1 <= M < 3 -> 4-6 (CAUTION); M >= 3 -> 7-10 (HIGH RISK).
    """
    if M < 1.0:
        s = 3.5 * M
    elif M < 3.0:
        s = 3.5 + 1.5 * (M - 1.0)
    else:
        s = 6.5 + (3.5 / 3.0) * (M - 3.0)
    score = int(np.floor(s + 0.5))  # round half up, same as C round() for positive values
    return max(0, min(10, score))


def run_ashrae_viitanen_model(humidity_series, temp_series):
    """
    Runs the full model (Equation 8 for growth + Equation 9 for decline) over
    a full series of hourly temperature/humidity readings, one hour at a time.
    Assumes each row in the input represents exactly 1 hour.
    """
    M = 0.0  # current mould index, starts at 0 (no mould)
    dry_streak_hours = 0  # how many consecutive hours conditions have been "too dry"
    scores = []  # we'll store the mould index at every hour here

    for T, RH in zip(temp_series, humidity_series):
        rhc = rh_crit(T)  # the humidity threshold for growth at this temperature

        if RH > rhc and T > 0:
            # ===== Conditions favour growth: apply Equation 8 =====
            dry_streak_hours = 0  # reset the "dry streak" counter since it's wet again

            mmax = m_max(RH, rhc)
            k1 = k1_factor(M, T, RH)
            k2 = k2_factor(M, mmax)

            exponent = (
                -0.68 * np.log(T) - 13.9 * np.log(RH) + 0.14 * W - 0.33 * SQ + 66.02
            )
            denom = 7 * np.exp(exponent)
            # The published growth rate is 1/(7*tm) with tm in weeks, i.e. a rate per DAY
            dM_per_day = (1.0 / denom) * k1 * k2 if denom > 0 else 0.0
            dM_per_hour = (
                dM_per_day / 24.0
            )  # convert daily rate to hourly rate (24 hours/day)

            M += dM_per_hour
        else:
            # ===== Conditions are too dry: apply Equation 9 (decline) =====
            dry_streak_hours += 1
            dM_per_hour = decline_rate_per_hour(dry_streak_hours)
            M += dM_per_hour

        # Mould index is always kept within the valid 0-6 range
        M = max(0.0, min(6.0, M))
        scores.append(M)

    return np.array(scores)


if __name__ == "__main__":
    # Find this script's own folder, so the CSV files are found regardless
    # of which folder the terminal happens to be in when we run this
    script_dir = os.path.dirname(os.path.abspath(__file__))
    input_path = os.path.join(script_dir, "synthetic_12month_dataset.csv")
    output_path = os.path.join(script_dir, "reference_ashrae_output.csv")

    print(f"Reading dataset from: {input_path}")
    df = pd.read_csv(input_path)

    print("Running the full ASHRAE/Viitanen model (no simplification)...")
    reference_scores = run_ashrae_viitanen_model(
        df["humidity_pct"].values, df["temperature_c"].values
    )

    # Add the calculated mould index as a new column and save the result
    df["ashrae_viitanen_index"] = reference_scores
    df["mould_score_010"] = [mould_score_from_index(m) for m in reference_scores]
    df.to_csv(output_path, index=False)

    print(f"\nDone! Result saved to: {output_path}")
    print(f"Final mould index (0-6 scale): {reference_scores[-1]:.3f}")
    print(f"Maximum mould index over the year: {reference_scores.max():.3f}")
    print(f"Maximum mould score (0-10) over the year: {df['mould_score_010'].max()}")
