set_project("SADO Controller")
set_version("0.1.0")
set_languages("c17")

add_rules("mode.debug", "mode.release")

add_requires("libsdl3")

target("SADO Controller")
    set_kind("binary")
    add_files("src/main.c")
    add_packages("libsdl3")