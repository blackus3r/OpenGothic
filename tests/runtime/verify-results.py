#!/usr/bin/env python3

import json
import pathlib
import sys


def fail(path: pathlib.Path, message: str) -> None:
    raise AssertionError(f"{path}: {message}")


def verify_enemy(path: pathlib.Path, data: dict) -> None:
    cases = data.get("cases", [])
    expected = ["WOLF", "BLOODFLY", "GOBBO_GREEN"]
    requested = [case.get("requested") for case in cases]
    if requested != expected:
        fail(path, f"expected cases {expected}, got {requested}")

    for case in cases:
        name = case["requested"]
        if case.get("assess_damage_callback") != "B_MM_ASSESSDAMAGE":
            fail(path, f"{name}: wrong assess-damage callback")
        if case.get("samples") != 8:
            fail(path, f"{name}: expected 8 samples")
        if case.get("fight_samples") != 8:
            fail(path, f"{name}: not every sample ran in FightAI")
        if case.get("callback_invocations") != 8:
            fail(path, f"{name}: assess-damage callback did not run for every sample")
        if not case.get("attack_animation_seen"):
            fail(path, f"{name}: no real attack animation was observed")
        deltas = case.get("hp_deltas", [])
        if len(deltas) != 8:
            fail(path, f"{name}: expected 8 HP deltas")
        if case.get("heal_events") != 0 or any(delta > 0 for delta in deltas):
            fail(path, f"{name}: observed an HP increase")
        if case.get("front_fov_false_samples") != 0:
            fail(path, f"{name}: front-cone perception flickered")
        if case.get("free_los_false_samples") != 0:
            fail(path, f"{name}: fixture LOS was obstructed")
        if not case.get("combat_reached") or not case.get("passed"):
            fail(path, f"{name}: combat case did not pass")

    legacy_failures = sum(
        case.get("legacy_head_model_fov_false_samples", 0) for case in cases
    )
    if legacy_failures == 0:
        fail(path, "legacy head/model FOV control did not reproduce the old bug")


def verify_orc(path: pathlib.Path, data: dict) -> None:
    cases = data.get("cases", [])
    expected = ["stock-senses", "controlled-see-only"]
    variants = [case.get("variant") for case in cases]
    if variants != expected:
        fail(path, f"expected variants {expected}, got {variants}")

    for case in cases:
        variant = case["variant"]
        if case.get("initial_angle_degrees", 0) < 170:
            fail(path, f"{variant}: player was not behind the Orc")
        if case.get("initial_fov"):
            fail(path, f"{variant}: directional FOV saw behind")
        if not case.get("initial_free_los"):
            fail(path, f"{variant}: free-LOS control was not visible")
        if case.get("npc_detection_radius_cm") != 3000:
            fail(path, f"{variant}: unexpected NPC detection radius")
        if case.get("nearby_other_npcs") != 0:
            fail(path, f"{variant}: unrelated NPCs were inside the test fixture")
        if case.get("nearest_other_npc_at_start_cm", 0) < 3000:
            fail(path, f"{variant}: nearest unrelated NPC was inside active range")
        expects_regular_perception = bool(
            case.get("assess_player_callback")
            and case.get("initial_sense_cone", 0) != 0
        ) or bool(
            case.get("assess_enemy_callback")
            and case.get("initial_sense_free", 0) != 0
        )
        if bool(case.get("initial_regular_perception_invoked")) != expects_regular_perception:
            fail(path, f"{variant}: regular perception result did not match its callbacks and senses")
        if not case.get("selector_acquired_player") or not case.get("turned"):
            fail(path, f"{variant}: stock detection path was not reproduced")
        if not case.get("fight_state_reached"):
            fail(path, f"{variant}: Orc never reached FightAI")
        if not case.get("passed"):
            fail(path, f"{variant}: case did not pass")

    stock = cases[0]
    if stock.get("initial_sense_cone", 0) == 0:
        fail(path, "stock-senses: smell/hearing control did not detect behind")
    if not (
        stock.get("fight_state_reached_before_probe")
        or stock.get("initial_regular_perception_invoked")
    ):
        fail(path, "stock-senses: no regular engine perception path ran")
    if (
        not stock.get("assess_player_callback")
        and not stock.get("assess_enemy_callback")
        and not stock.get("fight_state_reached_before_probe")
    ):
        fail(path, "stock-senses: no relevant perception callback was installed")

    controlled = cases[1]
    if controlled.get("senses_mask") != 1:
        fail(path, "controlled-see-only: senses were not isolated to sight")
    if controlled.get("initial_sense_cone") != 0:
        fail(path, "controlled-see-only: directional sensing unexpectedly succeeded")
    if controlled.get("initial_sense_free") != 1:
        fail(path, "controlled-see-only: free-LOS sensing did not succeed")
    if not (
        controlled.get("fight_state_reached_before_probe")
        or controlled.get("initial_regular_perception_invoked")
    ):
        fail(path, "controlled-see-only: no regular engine perception path ran")


def verify_sleep_placement(path: pathlib.Path, data: dict) -> None:
    case = data.get("case", {})
    required = (
        "found",
        "routine_time_set",
        "goto_bed_state_seen",
        "sleep_state_seen",
        "bed_fixture_seen",
        "bed_attached",
        "lie_pose_seen",
        "door_class_seen",
    )
    for key in required:
        if not case.get(key):
            fail(path, f"sleep placement did not observe {key}")
    if (case.get("routine_hour"), case.get("routine_minute")) != (0, 30):
        fail(path, "sleep placement did not enter Brian's 00:30 sleep routine")
    if case.get("door_semantics_seen"):
        fail(path, "BEDHIGH was incorrectly treated as a door interaction")
    if case.get("max_attachment_ground_delta_cm", 0) <= 50:
        fail(path, "sleep placement did not exercise the elevated BEDHIGH attachment")
    if case.get("attached_samples", 0) < 10:
        fail(path, "sleep placement collected too few attached samples")
    # The engine grounds the base once on attach and then leaves y alone while the entry
    # animation keeps moving x/z, so the base is not expected to track the floor below its
    # final spot. The regression is the NPC hovering over its support, which is checked
    # directly against the bed volume.
    if case.get("settled_samples", 0) < 10:
        fail(path, "sleep placement collected too few settled samples")
    if case.get("base_floating_samples") != 0:
        fail(path, "sleep placement observed the NPC base above the bed")
    if case.get("root_floating_samples") != 0:
        fail(path, "sleep placement observed the NPC root above the bed")
    if case.get("root_below_support_samples") != 0:
        fail(path, "sleep placement observed the NPC root sunk below the bed")
    if case.get("misplaced_samples") != 0:
        fail(path, "sleep placement observed the NPC beside its support")
    if case.get("horizontal_locomotion_samples") != 0:
        fail(path, "sleep placement observed horizontal walking")
    if case.get("max_base_above_support_cm", 999) > 0:
        fail(path, "sleep placement base hovered over the bed")
    if case.get("max_root_above_support_cm", 999) > 0:
        fail(path, "sleep placement root hovered over the bed")
    if case.get("max_root_horizontal_offset_cm", 999) > 20:
        fail(path, "sleep placement root was horizontally outside the bed support")
    if case.get("min_locomotion_upright_ratio", 0) < 0.45:
        fail(path, "sleep placement locomotion pose was not upright")
    if not case.get("passed"):
        fail(path, "sleep placement case did not pass")


def verify(path_text: str) -> None:
    path = pathlib.Path(path_text)
    with path.open(encoding="utf-8") as source:
        data = json.load(source)

    test_name = data.get("test")
    if test_name == "npc-sleep-placement":
        verify_sleep_placement(path, data)
        if not data.get("passed"):
            fail(path, "runtime harness reported FAIL")
        print(f"PASS {test_name}: {path}")
        return

    fixture = data.get("fixture", {})
    if not fixture.get("clear"):
        fail(path, "3D fixture clearance check failed")
    if fixture.get("clearance_score") != fixture.get("clearance_score_max"):
        fail(path, "fixture clearance score is incomplete")
    if fixture.get("npc_isolation_radius_cm") != 3000:
        fail(path, "fixture used an unexpected NPC isolation radius")
    if fixture.get("nearby_other_npcs") != 0:
        fail(path, "fixture contains unrelated NPCs")
    if fixture.get("nearest_other_npc_cm", 0) < 3000:
        fail(path, "fixture NPC clearance is below the active radius")
    if fixture.get("npc_quarantine_radius_cm") != 6000:
        fail(path, "fixture used an unexpected NPC quarantine radius")
    if not isinstance(fixture.get("quarantined_npcs"), int):
        fail(path, "fixture did not report its quarantined NPC count")

    if test_name == "enemy-heal-combat":
        verify_enemy(path, data)
    elif test_name == "orc-behind-detection":
        verify_orc(path, data)
    else:
        fail(path, f"unknown test {test_name!r}")

    if not data.get("passed"):
        fail(path, "runtime harness reported FAIL")
    print(f"PASS {test_name}: {path}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(f"usage: {sys.argv[0]} <result.json> [...]")
    try:
        for argument in sys.argv[1:]:
            verify(argument)
    except (AssertionError, OSError, json.JSONDecodeError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
