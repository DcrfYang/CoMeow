// mgmp_unlock_lists.h -- the names the game's `locked_content.gon` blacklists (generated from data/locked_content.gon; the ORDER is the wire format).
//
// A thing is locked while its name is on the blacklist AND absent from the save's unlocked set; every other name is always available. So the only
// names whose answer can differ between two players are these -- the host sends one bit per name, in this order. Never reorder; append only.
#pragma once

namespace mgmp {
namespace unlock_lists {

inline const char* const kAbilities[] = {
    "PathOfTheMage", "PathOfTheHunter", "PathOfTheThief", "PathOfTheFighter", "PathOfTheTank", "PathOfTheCleric",
    "PathOfTheButcher", "PathOfThePsychic", "PathOfTheTinkerer", "PathOfTheMonk", "PathOfTheDruid", "PathOfTheNecromancer",
    "PathOfTheJester", "PathOfTheVoid", "Pawbreaker", "BallOfSpiders", "HyperBeam", "Ethereal",
    "NailFlurry", "Suplex", "SummonShade", "Supernova", "MechSuit", "SliceAndDice",
    "SummonBear", "HundredHandSlap", "Metronome", "SmartMetronome", "RNGCannon", "Bump",
    "PowerUp",
};

inline const char* const kPassives[] = {
    "FightersSoul", "MagesSoul", "TanksSoul", "HuntersSoul", "ThiefsSoul", "ClericsSoul",
    "NecromancersSoul", "TinkerersSoul", "DruidsSoul", "MonksSoul", "ButchersSoul", "PsychicsSoul",
    "JestersSoul", "VoidSoul", "DualWield", "ThrillOfTheHunt", "EnergyStorm", "EvilPatron",
    "AlphaStrike", "Bouncer", "DeathIncarnate", "GravityWell", "ArmorSpecialist", "Indigestion",
    "SuicideSquad", "Unstoppable", "SkillShare", "SuperLuck", "Goofball",
};

inline const char* const kItems[] = {
    "FighterHelm", "FighterScar", "FighterShoulderPad", "BattleAxe", "RageJuice", "Steroids",
    "HunterCap", "HunterMonocle", "HunterQuiver", "InfinityArrow", "HuntersFlute", "BagOfSeeds",
    "MageHat", "MageRobe", "MageScarf", "StaffOfFlame", "RingOfFrost", "SpellBook",
    "ClericHat", "ClericTears", "ClericRelic", "AnointingOil", "HolyWater", "PrayerCard",
    "ThiefHood", "ThiefTattoo", "ThiefCloak", "LacedNeedle", "LuckyCoinPurse", "BagOfBags",
    "TankHelmet", "TankTattoo", "TankPads", "Girder", "TankJuice", "TankToy",
    "SpiderHat", "DeathMask", "LeechNecklace", "UnderworldStaff", "SatanicBible", "CambionConception",
    "TinfoilHat", "MysticEye", "DivinersCloth", "BentSpoon", "TarotDeck", "FriendshipBracelet",
    "ScrappersHat", "ScrappersMask", "ScrappersBackpack", "BallPeenHammer", "ScrapBag", "Fireworks",
    "FleshShroud", "IronJaw", "HookedNecklace", "ButchersCleaver", "Cookbook", "SackOfMeat",
    "MushroomHat", "GreenmanMask", "RingOfMushrooms", "RainStaff", "DruidsWhistle", "TinyCage",
    "HeadBrand", "FaceBrand", "PrayerBeads", "Bo", "EmptyHand", "TinyPebble",
    "CatEars", "CatWhiskers", "CatCollar", "LilKitty", "BallOfYarn", "SkillSplit",
    "CapAndBells", "ClownMakeup", "Ruffle", "NinnyStick", "ToyGun", "JokerCard",
    "CopycatHat", "CopycatMask", "CopycatScarf", "TheBoxCardboard", "TheBoxChest", "TheBox",
    "TheBlackBox", "MomsKnife", "ThrobbingCrown", "CrownOfChaos", "ChildsCrown", "TinasBellyButton",
    "TinasLarynx", "TinasFriend", "PyrophinasToenail", "ZaratanaTurd", "ScorchedEarth", "RoboticArm",
    "LiquidMetal", "HitlersToupe", "StevensGobbler", "StevensFartFace", "StevensShotgun", "StevensMustache",
    "StevensGristle", "StevensBagofRocks", "StevensHelmet", "StevensFriend", "StevenStone", "StevenMarrow",
    "StevensBottle", "StevensHat", "StevenHat1", "StevenMask1", "StevenNeck1", "StevenTrinket1",
    "StevenWeapon1", "StevensConsumable1", "StevenHat2", "StevenMask2", "StevenNeck2", "StevenTrinket2",
    "StevenWeapon2", "StevensConsumable2",
};

inline const char* const kLevelGroups[] = {
    "bigsharklevels",
};

inline const char* const kBosses[] = {
    "queenhippo", "jestercat", "gambit", "ratking", "bumblefoot", "infestedduo",
};

constexpr int kBossCount = 6;
constexpr int kAbilityCount = 31, kPassiveCount = 29, kItemCount = 128, kLevelGroupCount = 1;

} // namespace unlock_lists
} // namespace mgmp
