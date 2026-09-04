# OsmAnd fixtures

`town.osm` is a hand-written Overpass API answer: a synthetic town of three
residential streets by three lanes at Dublin's latitude, with a one-way lane,
a primary road, a motorway, a footway across the middle, a park, a lake, a
building, a river, a railway, a residential landuse ring, two named places,
and three ways the importer must throw away (an open landuse edge, a road
with a missing node, a proposed road).

`test/unit/test_osmand_app.c` imports it through the real
`nd_osm_import()`, loads the result through the real `nd_osm_map_load()`,
routes over it and renders it. A stand-in `curl` written by that test serves
the same file so the download path is exercised end to end with no network.

Every count the test asserts -- 38 nodes, 18 ways, 12 roads, 2 places -- is
derived from this file. Change the file and the test's numbers together.
