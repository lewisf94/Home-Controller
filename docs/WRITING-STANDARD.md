# Technical Writing Standard

## Requirement

All new or changed English technical text must follow ASD-STE100
Simplified Technical English (STE), Issue 9.

This requirement applies to these types of text:

- README files, guides, plans, procedures, and test instructions.
- Agent instructions, issue text, pull request text, and commit messages.
- Code comments, user help, warnings, errors, and diagnostic explanations.

Existing text enters this scope when a change affects its meaning.

The official standard is the source of truth. Use the
[Issue 9 standard](https://www.asd-ste100.org/assets/files/ASD-STE100_ISSUE9.pdf)
and the [official FAQ](https://www.asd-ste100.org/STE_faq.html).

This file is a project checklist. It does not replace the official standard
or its controlled dictionary.

## Exclusions

Do not change these items only to make them comply with STE:

- Source code, identifiers, commands, file paths, API names, and protocol names.
- Exact log output and data that comes from a device or a tool.
- Direct quotations, license text, generated text, and third-party text.

Preserve these items exactly when accuracy or compatibility requires their
original form.

## Writing Rules

1. Use an approved STE word for its approved meaning and part of speech.
2. Use project terms as technical nouns or technical verbs when necessary.
3. Use one term for one item or action. Do not use a synonym for variety.
4. Use a maximum of 20 words in each procedural sentence.
5. Use a maximum of 25 words in each descriptive sentence.
6. Give only one instruction in each sentence, unless two actions occur at the same time.
7. Use the imperative form for an instruction.
8. Put a necessary condition before the instruction.
9. Give only one topic in each descriptive sentence.
10. Give only one topic in each paragraph. Use a maximum of six sentences in a paragraph.
11. Use the active voice. Use the passive voice only when the agent is unknown in descriptive text.
12. Do not use contractions or omit necessary words.
13. Do not use slang, idioms, metaphors, or vague words.
14. Do not use an `-ing` form unless STE permits it or it is part of a technical noun.
15. Define an abbreviation at its first use. Use the same abbreviation after that definition.
16. Use numbered steps for a sequence. Give one main action in each step.
17. Put a warning or caution before the instruction that can cause the hazard.
18. State the possible result of a hazard and the action that prevents it.

## Project Terms

These product names and engineering terms are approved technical nouns:

- ESP-IDF, LVGL, FreeRTOS, LittleFS, NVS, PSRAM, Spotify, Sonos, and Home Assistant.
- ESP32, ESP32-P4, RP2040, MT6701, TMC6300, MCP23017, CYD, and Waveshare.
- Firmware, bootloader, build, flash, monitor, task, queue, heap, buffer, and register.

Use the spelling and capitalization in this list. Add a project term to this
section when readers can misunderstand it.

## Review

Before a commit, review all changed technical text against this file and the
official standard.

Do these checks:

1. Identify each sentence as procedural or descriptive.
2. Count the words in each sentence.
3. Check that each sentence has one instruction or one topic.
4. Check each general word against the STE dictionary.
5. Check that each project term has one consistent meaning.
6. Check that each condition, warning, and caution is in the correct position.
7. Read the text for ambiguity.

A language tool can help with this review. The writer and reviewer remain
responsible for compliance.

Do not state that text has formal or certified STE compliance unless a qualified
reviewer checks it against the full standard.
