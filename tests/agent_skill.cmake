# Checks that the agent skill describes this release, remains discoverable, and agrees
# with the public configuration documentation.
#
#   cmake -DKARU_SOURCE_DIR=<repository> -P tests/agent_skill.cmake

if(NOT KARU_SOURCE_DIR)
    message(FATAL_ERROR "pass -DKARU_SOURCE_DIR=<repository>")
endif()

set(skill "${KARU_SOURCE_DIR}/.claude/skills/karu")
file(READ "${KARU_SOURCE_DIR}/VERSION" version)
string(STRIP "${version}" version)
file(READ "${skill}/SKILL.md" entrypoint)

# Keep a real wrapper file rather than a repository symlink. Git commonly checks symlinks
# out as plain text files on Windows, which makes <skill>/SKILL.md undiscoverable.
set(agent_entrypoint "${KARU_SOURCE_DIR}/.agents/skills/karu/SKILL.md")
if(NOT EXISTS "${agent_entrypoint}")
    message(FATAL_ERROR ".agents/skills/karu/SKILL.md is missing")
endif()
file(READ "${agent_entrypoint}" agent_text)
string(FIND "${agent_text}" "](../../../.claude/skills/karu/SKILL.md)" canonical_link)
if(canonical_link EQUAL -1)
    message(FATAL_ERROR ".agents Karu wrapper does not link the canonical skill")
endif()

set(failures "")
file(GLOB references RELATIVE "${skill}" "${skill}/references/*.md")
foreach(reference IN LISTS references)
    string(FIND "${entrypoint}" "](${reference})" listed)
    if(listed EQUAL -1)
        list(APPEND failures "SKILL.md does not link ${reference}")
    endif()
endforeach()

# Every public HTML page with a version badge must describe this release.
file(GLOB html_pages "${KARU_SOURCE_DIR}/docs/*.html")
foreach(page IN LISTS html_pages)
    file(READ "${page}" html)
    string(FIND "${html}" "<span class=\"version\">" has_version)
    if(NOT has_version EQUAL -1)
        string(FIND "${html}" "<span class=\"version\">v${version}</span>" current_version)
        if(current_version EQUAL -1)
            get_filename_component(page_name "${page}" NAME)
            list(APPEND failures "${page_name} does not show v${version}")
        endif()
    endif()
endforeach()

# src/config_options.hpp is the accepted-name inventory. Both installed Markdown and
# the hand-written HTML reference must contain every spelling, including aliases.
file(READ "${KARU_SOURCE_DIR}/src/config_options.hpp" option_source)
file(READ "${KARU_SOURCE_DIR}/CONFIGURATION.md" markdown_configuration)
file(READ "${KARU_SOURCE_DIR}/docs/configuration.html" html_configuration)
string(REGEX MATCHALL "\"[A-Z][A-Z0-9_]+\"" option_literals "${option_source}")
list(REMOVE_DUPLICATES option_literals)
foreach(literal IN LISTS option_literals)
    string(REGEX REPLACE "^\"|\"$" "" option "${literal}")
    string(FIND "${markdown_configuration}" "`${option}`" markdown_has_option)
    if(markdown_has_option EQUAL -1)
        list(APPEND failures "CONFIGURATION.md does not document ${option}")
    endif()
    string(FIND "${html_configuration}" "<code>${option}</code>" html_has_option)
    if(html_has_option EQUAL -1)
        list(APPEND failures "docs/configuration.html does not document ${option}")
    endif()
endforeach()

file(GLOB_RECURSE documents RELATIVE "${skill}" "${skill}/*.md")
foreach(document IN LISTS documents)
    file(READ "${skill}/${document}" text)

    # Every statement that scopes guidance to a Karu release must track VERSION. This
    # catches stale references as well as the entrypoint without prescribing its wording.
    string(REGEX MATCHALL "[Kk]aru [0-9]+\\.[0-9]+\\.[0-9]+" version_mentions "${text}")
    foreach(mention IN LISTS version_mentions)
        string(REGEX REPLACE "^[Kk]aru " "" mentioned_version "${mention}")
        if(NOT mentioned_version STREQUAL version)
            list(APPEND failures
                "${document} describes Karu ${mentioned_version}, but VERSION is ${version}")
        endif()
    endforeach()

    get_filename_component(directory "${skill}/${document}" DIRECTORY)
    string(REGEX MATCHALL "\\]\\([^)#: ]+\\.md(#[^)]*)?\\)" links "${text}")
    foreach(match IN LISTS links)
        string(REGEX REPLACE "^\\]\\(([^)#]+\\.md).*$" "\\1" relative "${match}")
        if(NOT EXISTS "${directory}/${relative}")
            list(APPEND failures "${document} links missing ${relative}")
        endif()
    endforeach()
endforeach()

string(FIND "${entrypoint}" "karu ${version}" entrypoint_lower_version)
string(FIND "${entrypoint}" "Karu ${version}" entrypoint_upper_version)
if(entrypoint_lower_version EQUAL -1 AND entrypoint_upper_version EQUAL -1)
    list(APPEND failures "SKILL.md does not identify the Karu ${version} release it describes")
endif()

if(failures)
    list(JOIN failures "\n  " report)
    message(FATAL_ERROR "agent skill problems:\n  ${report}")
endif()
message(STATUS "agent skill and public configuration docs describe karu ${version}")
