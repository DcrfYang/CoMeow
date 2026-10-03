// mgmp_event_keys.h -- the save properties the random events read (generated from data/events/*.gon: every token, counter and flag a requirement or a conditional reward names).
//
// An event is drawn from the pool its REQUIREMENTS leave open -- has_token / not_has_token, counter_range / _minimum / _maximum, requires_flag -- and its
// rewards depend on the same counters, all of which are properties of the player's own save. Two players with different histories drew different events from
// the same seed, and an event's effects reach the shared simulation (weather, auras, the next fight's spawn queue). While a client follows the host through an
// event node these properties answer with the host's values (mgmp_unlocks). The ORDER is the wire format: append only.
#pragma once

namespace mgmp {
namespace event_keys {

inline const char* const kKeys[] = {
    "AdventureToken_BlueNeedle", "AdventureToken_HasRunFromDeath", "AdventureToken_HasTakenNeedle",
    "AdventureToken_Mirage1", "AdventureToken_Mirage2", "AdventureToken_MysteriousCave_FamiliarVoice",
    "AdventureToken_MysteriousJarRepeat", "AdventureToken_RedNeedle", "AdventureToken_StevenTryAgain",
    "AdventureToken_StevenTryAgain2", "AdventureToken_StevenTryAgain3", "AdventureToken_TrippedOnBigToe",
    "AdventureToken_UnmarkedGraveForced", "AdventureToken_YellowNeedle", "AlienInvasionUnlocked",
    "AlienOvergrowthUnlocked", "AntennaQuest_Orb", "AntennaQuest_Rift",
    "AntennaQuest_Volcano", "BonusBirdsKilled", "GeomagneticStormUnlocked",
    "HasPlayedMysteriousStranger", "HauntedNightUnlocked", "MeatWorldQuest_Gristle",
    "MeatWorldQuest_Leech", "MeteorShowerUnlocked", "RestlessDeadUnlocked",
    "RobotUprisingUnlocked", "SolarFlareUnlocked", "StrangeEggsUnlocked",
    "TheRift_UsedPyrophina", "TheRift_UsedZaratana", "TheShimmerUnlocked",
    "TimeMachineQuest_Began", "WorldEventLegacyCounter_CrackInTheWall", "WorldEventLegacyCounter_Jack",
    "WorldEventLegacyCounter_SealedCrypt", "WorldEventLegacyCounter_TestLegacyFoo", "WorldEventLegacyCounter_ToiletFlushes",
    "WorldEventLegacyCounter_VolcanoSacrifices", "WorldEventLegacyToken_CryptOpened", "WorldEventLegacyToken_HasRunFromDeath",
    "WorldEventLegacyToken_HeadInTireCompleted", "WorldEventLegacyToken_MomsKnife", "WorldEventLegacyToken_MonkeyPaw1",
    "WorldEventLegacyToken_MonkeyPaw2", "WorldEventLegacyToken_MonkeyPaw3", "WorldEventLegacyToken_MonkeyPaw4",
    "WorldEventLegacyToken_MonkeyPawGenes", "WorldEventLegacyToken_MonkeyPawItems", "WorldEventLegacyToken_MonkeyPawKnowledge",
    "WorldEventLegacyToken_MonkeyPawStrength", "WorldEventLegacyToken_StacyMutant", "WorldEventLegacyToken_StartDigging",
    "WorldEventLegacyToken_TapeRecorderDestroyed",
};
constexpr int kCount = 55;

} // namespace event_keys
} // namespace mgmp
