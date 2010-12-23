/*
 *  Phonebook access through D-Bus vCard and call history service
 *
 *  Copyright (C) 2010  Nokia Corporation
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <glib.h>
#include <dbus/dbus.h>
#include <openobex/obex.h>
#include <openobex/obex_const.h>

#include "log.h"
#include "obex.h"
#include "service.h"
#include "mimetype.h"
#include "phonebook.h"
#include "dbus.h"
#include "vcard.h"

#define TRACKER_SERVICE "org.freedesktop.Tracker1"
#define TRACKER_RESOURCES_PATH "/org/freedesktop/Tracker1/Resources"
#define TRACKER_RESOURCES_INTERFACE "org.freedesktop.Tracker1.Resources"

#define TRACKER_DEFAULT_CONTACT_ME "http://www.semanticdesktop.org/ontologies/2007/03/22/nco#default-contact-me"
#define AFFILATION_HOME "Home"
#define AFFILATION_WORK "Work"
#define ADDR_FIELD_AMOUNT 7
#define PULL_QUERY_COL_AMOUNT 26
#define COUNT_QUERY_COL_AMOUNT 1

#define COL_PHONE_AFF 0 /* work/home phone numbers */
#define COL_FULL_NAME 1
#define COL_FAMILY_NAME 2
#define COL_GIVEN_NAME 3
#define COL_ADDITIONAL_NAME 4
#define COL_NAME_PREFIX 5
#define COL_NAME_SUFFIX 6
#define COL_EMAIL_CONTACT 7 /*email's for other category */
#define COL_ADDR_AFF 8 /* addresses from affilation */
#define COL_ADDR_CONTACT 9 /* addresses from contacts */
#define COL_PHONE_CONTACT 10 /* phone numbers from contact's */
#define COL_BIRTH_DATE 11
#define COL_NICKNAME 12
#define COL_URL 13
#define COL_PHOTO 14
#define COL_ORG_ROLE 15
#define COL_UID 16
#define COL_TITLE 17
#define COL_AFF_TYPE 18
#define COL_ORG_NAME 19
#define COL_ORG_DEPARTMENT 20
#define COL_EMAIL_AFF 21 /* email's from affilation (work/home) */
#define COL_DATE 22
#define COL_SENT 23
#define COL_ANSWERED 24
#define CONTACTS_ID_COL 25
#define CONTACT_ID_PREFIX "contact:"

#define FAX_NUM_TYPE "http://www.semanticdesktop.org/ontologies/2007/03/22/nco#FaxNumber"
#define MOBILE_NUM_TYPE "http://www.semanticdesktop.org/ontologies/2007/03/22/nco#CellPhoneNumber"

#define MAIN_DELIM "\30" /* Main delimiter between phones, addresses, emails*/
#define SUB_DELIM "\31" /* Delimiter used in telephone number strings*/
#define MAX_FIELDS 100 /* Max amount of fields to be concatenated at once*/

#define CONTACTS_QUERY_ALL						\
"SELECT "								\
"(SELECT GROUP_CONCAT("							\
"nco:phoneNumber(?number), \"\30\")"					\
"WHERE {"								\
"	?_role nco:hasPhoneNumber ?number"				\
"}) "									\
"nco:fullname(?_contact) "						\
"nco:nameFamily(?_contact) "						\
"nco:nameGiven(?_contact) "						\
"nco:nameAdditional(?_contact) "					\
"nco:nameHonorificPrefix(?_contact) "					\
"nco:nameHonorificSuffix(?_contact) "					\
"(SELECT GROUP_CONCAT(?emailaddress_other, \"\30\") " 			\
	"WHERE {"							\
	"?_contact nco:hasEmailAddress "				\
			"[nco:emailAddress ?emailaddress_other]"	\
	"}) "								\
"(SELECT GROUP_CONCAT(fn:concat("					\
"tracker:coalesce(nco:pobox(?aff_addr), \"\"), \";\","			\
"tracker:coalesce(nco:extendedAddress(?aff_addr), \"\"), \";\","	\
"tracker:coalesce(nco:streetAddress(?aff_addr), \"\"), \";\","		\
"tracker:coalesce(nco:locality(?aff_addr), \"\"), \";\","		\
"tracker:coalesce(nco:region(?aff_addr), \"\"), \";\","			\
"tracker:coalesce(nco:postalcode(?aff_addr), \"\"), \";\","		\
"tracker:coalesce(nco:country(?aff_addr), \"\") ),\"\30\")"		\
"WHERE {"								\
"?_role nco:hasPostalAddress ?aff_addr"					\
"}) "									\
"(SELECT GROUP_CONCAT(fn:concat("					\
"tracker:coalesce(nco:pobox(?oth_addr), \"\"), \";\","			\
"tracker:coalesce(nco:extendedAddress(?oth_addr), \"\"), \";\","	\
"tracker:coalesce(nco:streetAddress(?oth_addr), \"\"), \";\","		\
"tracker:coalesce(nco:locality(?oth_addr), \"\"), \";\","		\
"tracker:coalesce(nco:region(?oth_addr), \"\"), \";\","			\
"tracker:coalesce(nco:postalcode(?oth_addr), \"\"), \";\","		\
"tracker:coalesce(nco:country(?oth_addr), \"\") ),\"\30\")"		\
"WHERE {"								\
"	?_contact nco:hasPostalAddress ?oth_addr"			\
"}) "									\
"(SELECT GROUP_CONCAT(fn:concat(rdf:type(?contact_number),"		\
"\"\31\", nco:phoneNumber(?contact_number)), \"\30\")"			\
"WHERE {"								\
"	?_contact nco:hasPhoneNumber ?contact_number"			\
"}) "									\
"nco:birthDate(?_contact) "						\
"nco:nickname(?_contact) "						\
"nco:url(?_contact) "							\
"nie:url(nco:photo(?_contact)) "					\
"nco:role(?_role) "							\
"nco:contactUID(?_contact) "						\
"nco:title(?_role) "							\
"rdfs:label(?_role) "							\
"nco:fullname(nco:org(?_role))"						\
"nco:department(?_role) "						\
"(SELECT GROUP_CONCAT(?emailaddress, \"\30\")"				\
"WHERE {"								\
"	?_role nco:hasEmailAddress [ nco:emailAddress ?emailaddress ]"	\
"}) "									\
"\"NOTACALL\" \"false\" \"false\" "					\
"?_contact "								\
"WHERE {"								\
"	?_contact a nco:PersonContact ;"				\
"	nco:nameFamily ?_key ."						\
"	OPTIONAL {?_contact nco:hasAffiliation ?_role .}"		\
"}"									\
"ORDER BY ?_key tracker:id(?_contact)"

#define CONTACTS_QUERY_ALL_LIST						\
	"SELECT ?c nco:nameFamily(?c) "					\
	"nco:nameGiven(?c) nco:nameAdditional(?c) "			\
	"nco:nameHonorificPrefix(?c) nco:nameHonorificSuffix(?c) "	\
	"nco:phoneNumber(?h) "						\
	"WHERE { "							\
		"?c a nco:PersonContact . "				\
	"OPTIONAL { ?c nco:hasPhoneNumber ?h . } "			\
	"OPTIONAL { "							\
		"?c nco:hasAffiliation ?a . "				\
		"?a nco:hasPhoneNumber ?h . "				\
	"} "								\
	"} GROUP BY ?c"

#define MISSED_CALLS_QUERY						\
"SELECT "								\
"(SELECT nco:phoneNumber(?role_number) "				\
	"WHERE {"							\
	"?_role nco:hasPhoneNumber ?role_number "			\
	"FILTER (?role_number = ?_number)"				\
"} GROUP BY nco:phoneNumber(?role_number) ) "				\
	"nco:fullname(?_contact) "					\
	"nco:nameFamily(?_contact) "					\
	"nco:nameGiven(?_contact) "					\
	"nco:nameAdditional(?_contact) "				\
	"nco:nameHonorificPrefix(?_contact) "				\
	"nco:nameHonorificSuffix(?_contact) "				\
"(SELECT GROUP_CONCAT(?emailaddress_other, \"\30\") " 			\
	"WHERE {"							\
	"?_contact nco:hasEmailAddress "				\
			"[nco:emailAddress ?emailaddress_other]"	\
	"}) "								\
"(SELECT GROUP_CONCAT(fn:concat("					\
	"tracker:coalesce(nco:pobox(?aff_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:extendedAddress(?aff_addr), \"\"), \";\","\
	"tracker:coalesce(nco:streetAddress(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:locality(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:region(?aff_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:postalcode(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:country(?aff_addr), \"\") ),\"\30\")"	\
	"WHERE {"							\
	"?_role nco:hasPostalAddress ?aff_addr"				\
	"}) "								\
"(SELECT GROUP_CONCAT(fn:concat("					\
	"tracker:coalesce(nco:pobox(?oth_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:extendedAddress(?oth_addr), \"\"), \";\","\
	"tracker:coalesce(nco:streetAddress(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:locality(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:region(?oth_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:postalcode(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:country(?oth_addr), \"\") ),\"\30\")"	\
	"WHERE {"							\
	"?_contact nco:hasPostalAddress ?oth_addr"			\
	"}) "								\
"(SELECT fn:concat(rdf:type(?contact_number),"				\
	"\"\31\", nco:phoneNumber(?contact_number))"			\
	"WHERE {"							\
	"{"								\
"		?_contact nco:hasPhoneNumber ?contact_number . "	\
"		FILTER (?contact_number = ?_number) "			\
"	} UNION { "							\
"		?_unb_contact nco:hasPhoneNumber ?contact_number . "	\
"	} "								\
"}GROUP BY nco:phoneNumber(?contact_number) ) "				\
	"nco:birthDate(?_contact) "					\
	"nco:nickname(?_contact) "					\
	"nco:url(?_contact) "						\
	"nie:url(nco:photo(?_contact)) "				\
	"nco:role(?_role) "						\
	"nco:contactUID(?_contact) "					\
	"nco:title(?_role) "						\
	"rdfs:label(?_role) "						\
	"nco:fullname(nco:org(?_role)) "				\
	"nco:department(?_role) "					\
"(SELECT GROUP_CONCAT(?emailaddress, \"\30\") "				\
	"WHERE { "							\
	"?_role nco:hasEmailAddress [ nco:emailAddress ?emailaddress ] "\
	"}) "								\
	"nmo:receivedDate(?_call) "					\
	"nmo:isSent(?_call) "						\
	"nmo:isAnswered(?_call) "					\
	"fn:concat(tracker:coalesce(?_ncontact, \"\"),"			\
	"tracker:coalesce(?_unb_contact, \"\"))"			\
	" "								\
"WHERE { "								\
"{ "									\
	"?_ncontact a nco:Contact . "					\
	"?_ncontact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:from ?_ncontact ; "					\
	"nmo:isAnswered false ;"					\
	"nmo:isSent false . "						\
	"?_contact a nco:PersonContact ; "				\
	"nco:hasPhoneNumber ?_number . "				\
	"OPTIONAL { ?_contact nco:hasAffiliation ?_role .} "		\
	"?_contact nco:nameFamily ?_key ."				\
"} UNION { "								\
	"?_ncontact a nco:Contact . "					\
	"?_ncontact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:from ?_ncontact ; "					\
	"nmo:isAnswered false ;"					\
	"nmo:isSent false . "						\
	"?_contact a nco:PersonContact . "				\
	"?_contact nco:nameFamily ?_key . "				\
	"?_contact nco:hasAffiliation ?_role . "			\
	"?_role nco:hasPhoneNumber ?_number . "				\
"} UNION { "								\
	"?_unb_contact a nco:Contact . "				\
	"?_unb_contact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:from ?_unb_contact ; "					\
	"nmo:isAnswered false ;"					\
	"nmo:isSent false . "						\
	"OPTIONAL {?_contact a nco:PersonContact ; "			\
	"nco:hasPhoneNumber ?_number . } "				\
	"OPTIONAL {?_contact a nco:PersonContact ; "			\
	"nco:hasAffiliation ?_role . "					\
	"?_role nco:hasPhoneNumber ?_number. } "			\
	"FILTER ( !bound(?_contact) && !bound(?_role) ) "		\
"} "									\
"} "									\
"ORDER BY DESC(nmo:sentDate(?_call)) "


#define MISSED_CALLS_LIST						\
	"SELECT ?c nco:nameFamily(?c) "					\
	"nco:nameGiven(?c) nco:nameAdditional(?c) "			\
	"nco:nameHonorificPrefix(?c) nco:nameHonorificSuffix(?c) "	\
	"nco:phoneNumber(?h) "						\
	"WHERE { "							\
	"{"								\
		"?c a nco:Contact . "					\
		"?c nco:hasPhoneNumber ?h . "				\
		"?call a nmo:Call ; "					\
		"nmo:from ?c ; "					\
		"nmo:isSent false ; "					\
		"nmo:isAnswered false ."				\
	"}UNION{"							\
		"?x a nco:Contact . "					\
		"?x nco:hasPhoneNumber ?h . "				\
		"?call a nmo:Call ; "					\
		"nmo:from ?x ; "					\
		"nmo:isSent false ; "					\
		"nmo:isAnswered false ."				\
		"?c a nco:PersonContact . "				\
		"?c nco:hasPhoneNumber ?h . "				\
	"} UNION { "							\
		"?x a nco:Contact . "					\
		"?x nco:hasPhoneNumber ?h . "				\
		"?call a nmo:Call ; "					\
		"nmo:from ?x ; "					\
		"nmo:isSent false ; "					\
		"nmo:isAnswered false ."				\
		"?c a nco:PersonContact . "				\
		"?c nco:hasAffiliation ?a . "				\
		"?a nco:hasPhoneNumber ?h . "				\
	"} "								\
	"} GROUP BY ?call ORDER BY DESC(nmo:receivedDate(?call))"

#define INCOMING_CALLS_QUERY						\
"SELECT "								\
"(SELECT nco:phoneNumber(?role_number) "				\
	"WHERE {"							\
"	?_role nco:hasPhoneNumber ?role_number"				\
"	FILTER (?role_number = ?_number)"				\
"} GROUP BY nco:phoneNumber(?role_number) ) "				\
	"nco:fullname(?_contact) "					\
	"nco:nameFamily(?_contact) "					\
	"nco:nameGiven(?_contact) "					\
	"nco:nameAdditional(?_contact) "				\
	"nco:nameHonorificPrefix(?_contact) "				\
	"nco:nameHonorificSuffix(?_contact) "				\
"(SELECT GROUP_CONCAT(?emailaddress_other, \"\30\") " 			\
	"WHERE {"							\
	"?_contact nco:hasEmailAddress "				\
			"[nco:emailAddress ?emailaddress_other]"	\
	"}) "								\
"(SELECT GROUP_CONCAT(fn:concat("					\
	"tracker:coalesce(nco:pobox(?aff_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:extendedAddress(?aff_addr), \"\"), \";\","\
	"tracker:coalesce(nco:streetAddress(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:locality(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:region(?aff_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:postalcode(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:country(?aff_addr), \"\") ),\"\30\")"	\
	"WHERE {"							\
	"?_role nco:hasPostalAddress ?aff_addr"				\
	"}) "								\
"(SELECT GROUP_CONCAT(fn:concat("					\
	"tracker:coalesce(nco:pobox(?oth_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:extendedAddress(?oth_addr), \"\"), \";\","\
	"tracker:coalesce(nco:streetAddress(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:locality(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:region(?oth_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:postalcode(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:country(?oth_addr), \"\") ),\"\30\")"	\
	"WHERE {"							\
	"?_contact nco:hasPostalAddress ?oth_addr"			\
	"}) "								\
"(SELECT fn:concat(rdf:type(?contact_number),"				\
	"\"\31\", nco:phoneNumber(?contact_number))"			\
	"WHERE {"							\
	"{"								\
"		?_contact nco:hasPhoneNumber ?contact_number . "	\
"		FILTER (?contact_number = ?_number) "			\
"	} UNION { "							\
"		?_unb_contact nco:hasPhoneNumber ?contact_number . "	\
"	} "								\
	"}GROUP BY nco:phoneNumber(?contact_number) ) "			\
	"nco:birthDate(?_contact) "					\
	"nco:nickname(?_contact) "					\
	"nco:url(?_contact) "						\
	"nie:url(nco:photo(?_contact)) "				\
	"nco:role(?_role) "						\
	"nco:contactUID(?_contact) "					\
	"nco:title(?_role) "						\
	"rdfs:label(?_role) "						\
	"nco:fullname(nco:org(?_role)) "				\
	"nco:department(?_role) "					\
"(SELECT GROUP_CONCAT(?emailaddress, \"\30\") "				\
	"WHERE { "							\
	"?_role nco:hasEmailAddress [ nco:emailAddress ?emailaddress ] "\
	"}) "								\
	"nmo:receivedDate(?_call) "					\
	"nmo:isSent(?_call) "						\
	"nmo:isAnswered(?_call) "					\
	"fn:concat(tracker:coalesce(?_ncontact, \"\"),"			\
	"tracker:coalesce(?_unb_contact, \"\"))"			\
	" "								\
"WHERE { "								\
"{ "									\
	"?_ncontact a nco:Contact . "					\
	"?_ncontact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:from ?_ncontact ; "					\
	"nmo:isAnswered true ;"						\
	"nmo:isSent false . "						\
	"?_contact a nco:PersonContact ; "				\
	"nco:hasPhoneNumber ?_number . "				\
	"OPTIONAL { ?_contact nco:hasAffiliation ?_role .} "		\
	"?_contact nco:nameFamily ?_key ."				\
"} UNION { "								\
	"?_ncontact a nco:Contact . "					\
	"?_ncontact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:from ?_ncontact ; "					\
	"nmo:isAnswered true ;"						\
	"nmo:isSent false . "						\
	"?_contact a nco:PersonContact . "				\
	"?_contact nco:nameFamily ?_key . "				\
	"?_contact nco:hasAffiliation ?_role . "			\
	"?_role nco:hasPhoneNumber ?_number . "				\
"} UNION { "								\
	"?_unb_contact a nco:Contact . "				\
	"?_unb_contact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:from ?_unb_contact ; "					\
	"nmo:isAnswered true ;"						\
	"nmo:isSent false . "						\
	"OPTIONAL {?_contact a nco:PersonContact ; "			\
	"nco:hasPhoneNumber ?_number . } "				\
	"OPTIONAL {?_contact a nco:PersonContact ; "			\
	"nco:hasAffiliation ?_role . "					\
	"?_role nco:hasPhoneNumber ?_number. } "			\
	"FILTER ( !bound(?_contact) && !bound(?_role) ) "		\
"} "									\
"} "\
"ORDER BY DESC(nmo:sentDate(?_call)) "

#define INCOMING_CALLS_LIST						\
	"SELECT ?c nco:nameFamily(?c) "					\
	"nco:nameGiven(?c) nco:nameAdditional(?c) "			\
	"nco:nameHonorificPrefix(?c) nco:nameHonorificSuffix(?c) "	\
	"nco:phoneNumber(?h) "						\
	"WHERE { "							\
	"{"								\
		"?c a nco:Contact . "					\
		"?c nco:hasPhoneNumber ?h . "				\
		"?call a nmo:Call ; "					\
		"nmo:from ?c ; "					\
		"nmo:isSent false ; "					\
		"nmo:isAnswered true ."					\
	"} UNION { "							\
		"?x a nco:Contact . "					\
		"?x nco:hasPhoneNumber ?h ."				\
		"?call a nmo:Call ; "					\
		"nmo:from ?x ; "					\
		"nmo:isSent false ; "					\
		"nmo:isAnswered true ."					\
		"?c a nco:PersonContact . "				\
		"?c nco:hasPhoneNumber ?h ."				\
	"}UNION { "							\
		"?x a nco:Contact . "					\
		"?x nco:hasPhoneNumber ?h ."				\
		"?call a nmo:Call ; "					\
		"nmo:from ?x ; "					\
		"nmo:isSent false ; "					\
		"nmo:isAnswered true ."					\
		"?c a nco:PersonContact . "				\
		"?c nco:hasAffiliation ?a . "				\
		"?a nco:hasPhoneNumber ?h . "				\
	"}"								\
	"} GROUP BY ?call ORDER BY DESC(nmo:receivedDate(?call))"

#define OUTGOING_CALLS_QUERY						\
"SELECT "								\
"(SELECT nco:phoneNumber(?role_number) "				\
	"WHERE {"							\
"	?_role nco:hasPhoneNumber ?role_number"				\
"	FILTER (?role_number = ?_number)"				\
"} GROUP BY nco:phoneNumber(?role_number) ) "				\
	"nco:fullname(?_contact) "					\
	"nco:nameFamily(?_contact) "					\
	"nco:nameGiven(?_contact) "					\
	"nco:nameAdditional(?_contact) "				\
	"nco:nameHonorificPrefix(?_contact) "				\
	"nco:nameHonorificSuffix(?_contact) "				\
"(SELECT GROUP_CONCAT(?emailaddress_other, \"\30\") " 			\
	"WHERE {"							\
	"?_contact nco:hasEmailAddress "				\
			"[nco:emailAddress ?emailaddress_other]"	\
	"}) "								\
"(SELECT GROUP_CONCAT(fn:concat("					\
	"tracker:coalesce(nco:pobox(?aff_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:extendedAddress(?aff_addr), \"\"), \";\","\
	"tracker:coalesce(nco:streetAddress(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:locality(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:region(?aff_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:postalcode(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:country(?aff_addr), \"\") ),\"\30\")"	\
	"WHERE {"							\
	"?_role nco:hasPostalAddress ?aff_addr"				\
	"}) "								\
"(SELECT GROUP_CONCAT(fn:concat("					\
	"tracker:coalesce(nco:pobox(?oth_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:extendedAddress(?oth_addr), \"\"), \";\","\
	"tracker:coalesce(nco:streetAddress(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:locality(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:region(?oth_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:postalcode(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:country(?oth_addr), \"\") ),\"\30\")"	\
	"WHERE {"							\
	"?_contact nco:hasPostalAddress ?oth_addr"			\
	"}) "								\
"(SELECT fn:concat(rdf:type(?contact_number),"				\
	"\"\31\", nco:phoneNumber(?contact_number))"			\
	"WHERE {"							\
	"{"								\
"		?_contact nco:hasPhoneNumber ?contact_number . "	\
"		FILTER (?contact_number = ?_number) "			\
"	} UNION { "							\
"		?_unb_contact nco:hasPhoneNumber ?contact_number . "	\
"	} "								\
	"}GROUP BY nco:phoneNumber(?contact_number) ) "			\
	"nco:birthDate(?_contact) "					\
	"nco:nickname(?_contact) "					\
	"nco:url(?_contact) "						\
	"nie:url(nco:photo(?_contact)) "				\
	"nco:role(?_role) "						\
	"nco:contactUID(?_contact) "					\
	"nco:title(?_role) "						\
	"rdfs:label(?_role) "						\
	"nco:fullname(nco:org(?_role)) "				\
	"nco:department(?_role) "					\
"(SELECT GROUP_CONCAT(?emailaddress, \"\30\") "				\
	"WHERE { "							\
	"?_role nco:hasEmailAddress [ nco:emailAddress ?emailaddress ] "\
	"}) "								\
	"nmo:receivedDate(?_call) "					\
	"nmo:isSent(?_call) "						\
	"nmo:isAnswered(?_call) "					\
	"fn:concat(tracker:coalesce(?_ncontact, \"\"),"			\
	"tracker:coalesce(?_unb_contact, \"\"))"			\
	" "								\
"WHERE { "								\
"{ "									\
	"?_ncontact a nco:Contact . "					\
	"?_ncontact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:to ?_ncontact ; "						\
	"nmo:isSent true . "						\
	"?_contact a nco:PersonContact ; "				\
	"nco:hasPhoneNumber ?_number . "				\
	"OPTIONAL { ?_contact nco:hasAffiliation ?_role .} "		\
	"?_contact nco:nameFamily ?_key ."				\
"} UNION { "								\
	"?_ncontact a nco:Contact . "					\
	"?_ncontact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:to ?_ncontact ; "						\
	"nmo:isSent true . "						\
	"?_contact a nco:PersonContact . "				\
	"?_contact nco:nameFamily ?_key . "				\
	"?_contact nco:hasAffiliation ?_role . "			\
	"?_role nco:hasPhoneNumber ?_number . "				\
"} UNION { "								\
	"?_unb_contact a nco:Contact . "				\
	"?_unb_contact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:to ?_unb_contact ; "					\
	"nmo:isSent true . "						\
	"OPTIONAL {?_contact a nco:PersonContact ; "			\
	"nco:hasPhoneNumber ?_number . } "				\
	"OPTIONAL {?_contact a nco:PersonContact ; "			\
	"nco:hasAffiliation ?_role . "					\
	"?_role nco:hasPhoneNumber ?_number. } "			\
	"FILTER ( !bound(?_contact) && !bound(?_role) ) "		\
"} "									\
"} "									\
"ORDER BY DESC(nmo:sentDate(?_call)) "

#define OUTGOING_CALLS_LIST						\
	"SELECT ?c nco:nameFamily(?c) "					\
	"nco:nameGiven(?c) nco:nameAdditional(?c) "			\
	"nco:nameHonorificPrefix(?c) nco:nameHonorificSuffix(?c) "	\
	"nco:phoneNumber(?h) "						\
	"WHERE { "							\
	"{"								\
		"?c a nco:Contact . "					\
		"?c nco:hasPhoneNumber ?h . "				\
		"?call a nmo:Call ; "					\
		"nmo:to ?c ; "						\
		"nmo:isSent true . "					\
	"} UNION {"							\
		"?x a nco:Contact . "					\
		"?x nco:hasPhoneNumber ?h . "				\
		"?call a nmo:Call ; "					\
		"nmo:to ?x ; "						\
		"nmo:isSent true . "					\
		"?c a nco:PersonContact . "				\
		"?c nco:hasPhoneNumber ?h . "				\
	"} UNION {"							\
		"?x a nco:Contact . "					\
		"?x nco:hasPhoneNumber ?h . "				\
		"?call a nmo:Call ; "					\
		"nmo:to ?x ; "						\
		"nmo:isSent true . "					\
		"?c a nco:PersonContact . "				\
		"?c nco:hasAffiliation ?a . "				\
		"?a nco:hasPhoneNumber ?h . "				\
	"}"								\
	"} GROUP BY ?call ORDER BY DESC(nmo:sentDate(?call))"

#define COMBINED_CALLS_QUERY						\
"SELECT "								\
"(SELECT nco:phoneNumber(?role_number) "				\
	"WHERE {"							\
"	?_role nco:hasPhoneNumber ?role_number"				\
"	FILTER (?role_number = ?_number)"				\
"} GROUP BY nco:phoneNumber(?role_number) ) "				\
	"nco:fullname(?_contact) "					\
	"nco:nameFamily(?_contact) "					\
	"nco:nameGiven(?_contact) "					\
	"nco:nameAdditional(?_contact) "				\
	"nco:nameHonorificPrefix(?_contact) "				\
	"nco:nameHonorificSuffix(?_contact) "				\
"(SELECT GROUP_CONCAT(?emailaddress_other, \"\30\") " 			\
	"WHERE {"							\
	"?_contact nco:hasEmailAddress "				\
			"[nco:emailAddress ?emailaddress_other]"	\
	"}) "								\
"(SELECT GROUP_CONCAT(fn:concat("					\
	"tracker:coalesce(nco:pobox(?aff_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:extendedAddress(?aff_addr), \"\"), \";\","\
	"tracker:coalesce(nco:streetAddress(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:locality(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:region(?aff_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:postalcode(?aff_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:country(?aff_addr), \"\") ),\"\30\")"	\
	"WHERE {"							\
	"?_role nco:hasPostalAddress ?aff_addr"				\
	"}) "								\
"(SELECT GROUP_CONCAT(fn:concat("					\
	"tracker:coalesce(nco:pobox(?oth_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:extendedAddress(?oth_addr), \"\"), \";\","\
	"tracker:coalesce(nco:streetAddress(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:locality(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:region(?oth_addr), \"\"), \";\","		\
	"tracker:coalesce(nco:postalcode(?oth_addr), \"\"), \";\","	\
	"tracker:coalesce(nco:country(?oth_addr), \"\") ),\"\30\")"	\
	"WHERE {"							\
	"?_contact nco:hasPostalAddress ?oth_addr"			\
	"}) "								\
"(SELECT fn:concat(rdf:type(?contact_number),"				\
	"\"\31\", nco:phoneNumber(?contact_number))"			\
	"WHERE {"							\
	"{"								\
"		?_contact nco:hasPhoneNumber ?contact_number . "	\
"		FILTER (?contact_number = ?_number) "			\
"	} UNION { "							\
"		?_unb_contact nco:hasPhoneNumber ?contact_number . "	\
"	} "								\
	"}GROUP BY nco:phoneNumber(?contact_number) ) "			\
	"nco:birthDate(?_contact) "					\
	"nco:nickname(?_contact) "					\
	"nco:url(?_contact) "						\
	"nie:url(nco:photo(?_contact)) "				\
	"nco:role(?_role) "						\
	"nco:contactUID(?_contact) "					\
	"nco:title(?_role) "						\
	"rdfs:label(?_role) "						\
	"nco:fullname(nco:org(?_role)) "				\
	"nco:department(?_role) "					\
"(SELECT GROUP_CONCAT(?emailaddress, \"\30\") "				\
	"WHERE { "							\
	"?_role nco:hasEmailAddress [ nco:emailAddress ?emailaddress ] "\
	"}) "								\
	"nmo:receivedDate(?_call) "					\
	"nmo:isSent(?_call) "						\
	"nmo:isAnswered(?_call) "					\
	"fn:concat(tracker:coalesce(?_ncontact, \"\"),"			\
	"tracker:coalesce(?_unb_contact, \"\"))"			\
	" "								\
"WHERE { "								\
"{ "									\
	"?_ncontact a nco:Contact . "					\
	"?_ncontact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:to ?_ncontact ; "						\
	"nmo:isSent true . "						\
	"?_contact a nco:PersonContact ; "				\
	"nco:hasPhoneNumber ?_number . "				\
	"OPTIONAL { ?_contact nco:hasAffiliation ?_role .} "		\
	"?_contact nco:nameFamily ?_key ."				\
"} UNION { "								\
	"?_ncontact a nco:Contact . "					\
	"?_ncontact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:to ?_ncontact ; "						\
	"nmo:isSent true . "						\
	"?_contact a nco:PersonContact . "				\
	"?_contact nco:nameFamily ?_key . "				\
	"?_contact nco:hasAffiliation ?_role . "			\
	"?_role nco:hasPhoneNumber ?_number . "				\
"} UNION { "								\
	"?_unb_contact a nco:Contact . "				\
	"?_unb_contact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:to ?_unb_contact ; "					\
	"nmo:isSent true . "						\
	"OPTIONAL {?_contact a nco:PersonContact ; "			\
	"nco:hasPhoneNumber ?_number . } "				\
	"OPTIONAL {?_contact a nco:PersonContact ; "			\
	"nco:hasAffiliation ?_role . "					\
	"?_role nco:hasPhoneNumber ?_number. } "			\
	"FILTER ( !bound(?_contact) && !bound(?_role) ) "		\
"} UNION { "								\
	"?_ncontact a nco:Contact . "					\
	"?_ncontact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:from ?_ncontact ; "					\
	"nmo:isSent false . "						\
	"?_contact a nco:PersonContact ; "				\
	"nco:hasPhoneNumber ?_number . "				\
	"OPTIONAL { ?_contact nco:hasAffiliation ?_role .} "		\
	"?_contact nco:nameFamily ?_key ."				\
"} UNION { "								\
	"?_ncontact a nco:Contact . "					\
	"?_ncontact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:from ?_ncontact ; "					\
	"nmo:isSent false . "						\
	"?_contact a nco:PersonContact . "				\
	"?_contact nco:nameFamily ?_key . "				\
	"?_contact nco:hasAffiliation ?_role . "			\
	"?_role nco:hasPhoneNumber ?_number . "				\
"} UNION { "								\
	"?_unb_contact a nco:Contact . "				\
	"?_unb_contact nco:hasPhoneNumber ?_number . "			\
	"?_call a nmo:Call ; "						\
	"nmo:from ?_unb_contact ; "					\
	"nmo:isSent false . "						\
	"OPTIONAL {?_contact a nco:PersonContact ; "			\
	"nco:hasPhoneNumber ?_number . } "				\
	"OPTIONAL {?_contact a nco:PersonContact ; "			\
	"nco:hasAffiliation ?_role . "					\
	"?_role nco:hasPhoneNumber ?_number. } "			\
	"FILTER ( !bound(?_contact) && !bound(?_role) ) "		\
"} "									\
"} "									\
"ORDER BY DESC(nmo:sentDate(?_call)) "

#define COMBINED_CALLS_LIST						\
	"SELECT ?c nco:nameFamily(?c) nco:nameGiven(?c) "		\
	"nco:nameAdditional(?c) nco:nameHonorificPrefix(?c) "		\
	"nco:nameHonorificSuffix(?c) nco:phoneNumber(?h) "		\
	"WHERE { "							\
	"	{ "							\
			"?c a nco:Contact . "				\
			"?c nco:hasPhoneNumber ?h . "			\
			"?call a nmo:Call ; "				\
			"nmo:to ?c ; "					\
			"nmo:isSent true . "				\
		"} UNION {"						\
			"?x a nco:Contact . "				\
			"?x nco:hasPhoneNumber ?h . "			\
			"?call a nmo:Call ; "				\
			"nmo:to ?x ; "					\
			"nmo:isSent true . "				\
			"?c a nco:PersonContact . "			\
			"?c nco:hasPhoneNumber ?h . "			\
		"} UNION {"						\
			"?x a nco:Contact . "				\
			"?x nco:hasPhoneNumber ?h . "			\
			"?call a nmo:Call ; "				\
			"nmo:to ?x ; "					\
			"nmo:isSent true . "				\
			"?c a nco:PersonContact . "			\
			"?c nco:hasAffiliation ?a . "			\
			"?a nco:hasPhoneNumber ?h . "			\
		"}UNION {"						\
			"?c a nco:Contact . "				\
			"?c nco:hasPhoneNumber ?h . "			\
			"?call a nmo:Call ; "				\
			"nmo:from ?c ; "				\
			"nmo:isSent false . "				\
		"} UNION {"						\
			"?x a nco:Contact . "				\
			"?x nco:hasPhoneNumber ?h . "			\
			"?call a nmo:Call ; "				\
			"nmo:from ?x ; "				\
			"nmo:isSent false . "				\
			"?c a nco:PersonContact . "			\
			"?c nco:hasPhoneNumber ?h . "			\
		"} UNION {"						\
			"?x a nco:Contact . "				\
			"?x nco:hasPhoneNumber ?h . "			\
			"?call a nmo:Call ; "				\
			"nmo:from ?x ; "				\
			"nmo:isSent false . "				\
			"?c a nco:PersonContact . "			\
			"?c nco:hasAffiliation ?a . "			\
			"?a nco:hasPhoneNumber ?h . "			\
		"}"							\
	"} GROUP BY ?call ORDER BY DESC(nmo:receivedDate(?call))"

#define CONTACTS_QUERY_FROM_URI						\
"SELECT "								\
"(SELECT GROUP_CONCAT("							\
"nco:phoneNumber(?number), \"\30\")"					\
"WHERE {"								\
"	?_role nco:hasPhoneNumber ?number"				\
"}) "									\
"nco:fullname(<%s>) "							\
"nco:nameFamily(<%s>) "							\
"nco:nameGiven(<%s>) "							\
"nco:nameAdditional(<%s>) "						\
"nco:nameHonorificPrefix(<%s>) "					\
"nco:nameHonorificSuffix(<%s>) "					\
"(SELECT GROUP_CONCAT(?emailaddress_other, \"\30\")"			\
"WHERE {"								\
"	<%s> nco:hasEmailAddress [nco:emailAddress ?emailaddress_other]"\
"}) "									\
"(SELECT GROUP_CONCAT(fn:concat("					\
"tracker:coalesce(nco:pobox(?aff_addr), \"\"), \";\","			\
"tracker:coalesce(nco:extendedAddress(?aff_addr), \"\"), \";\","	\
"tracker:coalesce(nco:streetAddress(?aff_addr), \"\"), \";\","		\
"tracker:coalesce(nco:locality(?aff_addr), \"\"), \";\","		\
"tracker:coalesce(nco:region(?aff_addr), \"\"), \";\","			\
"tracker:coalesce(nco:postalcode(?aff_addr), \"\"), \";\","		\
"tracker:coalesce(nco:country(?aff_addr), \"\") ),\"\30\")"		\
"WHERE {"								\
"?_role nco:hasPostalAddress ?aff_addr"					\
"}) "									\
"(SELECT GROUP_CONCAT(fn:concat("					\
"tracker:coalesce(nco:pobox(?oth_addr), \"\"), \";\","			\
"tracker:coalesce(nco:extendedAddress(?oth_addr), \"\"), \";\","	\
"tracker:coalesce(nco:streetAddress(?oth_addr), \"\"), \";\","		\
"tracker:coalesce(nco:locality(?oth_addr), \"\"), \";\","		\
"tracker:coalesce(nco:region(?oth_addr), \"\"), \";\","			\
"tracker:coalesce(nco:postalcode(?oth_addr), \"\"), \";\","		\
"tracker:coalesce(nco:country(?oth_addr), \"\") ),\"\30\")"		\
"WHERE {"								\
"	<%s> nco:hasPostalAddress ?oth_addr"				\
"}) "									\
"(SELECT GROUP_CONCAT(fn:concat(rdf:type(?contact_number),"		\
"\"\31\", nco:phoneNumber(?contact_number)), \"\30\")"			\
"WHERE {"								\
"	<%s> nco:hasPhoneNumber ?contact_number"			\
"}) "									\
"nco:birthDate(<%s>) "							\
"nco:nickname(<%s>) "							\
"nco:url(<%s>) "							\
"nie:url(nco:photo(<%s>)) "						\
"nco:role(?_role) "							\
"nco:contactUID(<%s>) "							\
"nco:title(?_role) "							\
"rdfs:label(?_role) "							\
"nco:fullname(nco:org(?_role))"						\
"nco:department(?_role) "						\
"(SELECT GROUP_CONCAT(?emailaddress, \"\30\")"				\
"WHERE {"								\
"	?_role nco:hasEmailAddress [ nco:emailAddress ?emailaddress ]"	\
"}) "									\
"\"NOTACALL\" \"false\" \"false\" "					\
"<%s> "									\
"WHERE {"								\
"	<%s> a nco:PersonContact ;"					\
"	nco:nameFamily ?_key ."						\
"	OPTIONAL {<%s> nco:hasAffiliation ?_role .}"			\
"}"									\
"ORDER BY ?_key tracker:id(<%s>)"

#define CONTACTS_OTHER_QUERY_FROM_URI					\
	"SELECT \"\" \"\" \"\" \"\" \"\" \"\" \"\" \"\" \"\" \"\" "	\
	"fn:concat(\"TYPE_OTHER\", \"\31\", nco:phoneNumber(?t)) \"\" "	\
	"\"\" \"\" \"\" \"\" \"\" \"\" \"\" \"\" \"\" \"\" "		\
	" \"NOTACALL\" \"false\" \"false\" <%s> "	\
	"WHERE { "							\
		"<%s> a nco:Contact . "					\
		"OPTIONAL { <%s> nco:hasPhoneNumber ?t . } "		\
	"} "

#define CONTACTS_COUNT_QUERY						\
	"SELECT COUNT(?c) "						\
	"WHERE {"							\
		"?c a nco:PersonContact ."				\
		"FILTER (regex(str(?c), \"contact:\") || "		\
		"regex(str(?c), \"nco#default-contact-me\"))"		\
	"}"

#define MISSED_CALLS_COUNT_QUERY					\
	"SELECT COUNT(?call) WHERE {"					\
		"?c a nco:Contact ;"					\
		"nco:hasPhoneNumber ?h ."				\
		"?call a nmo:Call ;"					\
		"nmo:isSent false ;"					\
		"nmo:from ?c ;"						\
		"nmo:isAnswered false ."				\
	"}"

#define INCOMING_CALLS_COUNT_QUERY					\
	"SELECT COUNT(?call) WHERE {"					\
		"?c a nco:Contact ;"					\
		"nco:hasPhoneNumber ?h ."				\
		"?call a nmo:Call ;"					\
		"nmo:isSent false ;"					\
		"nmo:from ?c ;"						\
		"nmo:isAnswered true ."					\
	"}"

#define OUTGOING_CALLS_COUNT_QUERY					\
	"SELECT COUNT(?call) WHERE {"					\
		"?c a nco:Contact ;"					\
		"nco:hasPhoneNumber ?h ."				\
		"?call a nmo:Call ;"					\
		"nmo:isSent true ;"					\
		"nmo:to ?c ."						\
	"}"

#define COMBINED_CALLS_COUNT_QUERY					\
	"SELECT COUNT(?call) WHERE {"					\
	"{"								\
		"?c a nco:Contact ;"					\
		"nco:hasPhoneNumber ?h ."				\
		"?call a nmo:Call ;"					\
		"nmo:isSent true ;"					\
		"nmo:to ?c ."						\
	"}UNION {"							\
		"?c a nco:Contact ;"					\
		"nco:hasPhoneNumber ?h ."				\
		"?call a nmo:Call ;"					\
		"nmo:from ?c ."						\
	"}"								\
	"}"

typedef void (*reply_list_foreach_t) (char **reply, int num_fields,
							void *user_data);

struct pending_reply {
	reply_list_foreach_t callback;
	void *user_data;
	int num_fields;
	GDestroyNotify destroy;
};

struct contact_data {
	char *id;
	struct phonebook_contact *contact;
};

struct phonebook_data {
	phonebook_cb cb;
	void *user_data;
	int index;
	gboolean vcardentry;
	const struct apparam_field *params;
	GSList *contacts;
};

struct cache_data {
	phonebook_cache_ready_cb ready_cb;
	phonebook_entry_cb entry_cb;
	void *user_data;
	GString *listing;
	int index;
};

struct phonebook_index {
	GArray *phonebook;
	int index;
};

static DBusConnection *connection = NULL;

static const char *name2query(const char *name)
{
	if (g_str_equal(name, "telecom/pb.vcf"))
		return CONTACTS_QUERY_ALL;
	else if (g_str_equal(name, "telecom/ich.vcf"))
		return INCOMING_CALLS_QUERY;
	else if (g_str_equal(name, "telecom/och.vcf"))
		return OUTGOING_CALLS_QUERY;
	else if (g_str_equal(name, "telecom/mch.vcf"))
		return MISSED_CALLS_QUERY;
	else if (g_str_equal(name, "telecom/cch.vcf"))
		return COMBINED_CALLS_QUERY;

	return NULL;
}

static const char *name2count_query(const char *name)
{
	if (g_str_equal(name, "telecom/pb.vcf"))
		return CONTACTS_COUNT_QUERY;
	else if (g_str_equal(name, "telecom/ich.vcf"))
		return INCOMING_CALLS_COUNT_QUERY;
	else if (g_str_equal(name, "telecom/och.vcf"))
		return OUTGOING_CALLS_COUNT_QUERY;
	else if (g_str_equal(name, "telecom/mch.vcf"))
		return MISSED_CALLS_COUNT_QUERY;
	else if (g_str_equal(name, "telecom/cch.vcf"))
		return COMBINED_CALLS_COUNT_QUERY;

	return NULL;
}

static gboolean folder_is_valid(const char *folder)
{
	if (folder == NULL)
		return FALSE;

	if (g_str_equal(folder, "/"))
		return TRUE;
	else if (g_str_equal(folder, "/telecom"))
		return TRUE;
	else if (g_str_equal(folder, "/telecom/pb"))
		return TRUE;
	else if (g_str_equal(folder, "/telecom/ich"))
		return TRUE;
	else if (g_str_equal(folder, "/telecom/och"))
		return TRUE;
	else if (g_str_equal(folder, "/telecom/mch"))
		return TRUE;
	else if (g_str_equal(folder, "/telecom/cch"))
		return TRUE;

	return FALSE;
}

static const char *folder2query(const char *folder)
{
	if (g_str_equal(folder, "/telecom/pb"))
		return CONTACTS_QUERY_ALL_LIST;
	else if (g_str_equal(folder, "/telecom/ich"))
		return INCOMING_CALLS_LIST;
	else if (g_str_equal(folder, "/telecom/och"))
		return OUTGOING_CALLS_LIST;
	else if (g_str_equal(folder, "/telecom/mch"))
		return MISSED_CALLS_LIST;
	else if (g_str_equal(folder, "/telecom/cch"))
		return COMBINED_CALLS_LIST;

	return NULL;
}

static char **string_array_from_iter(DBusMessageIter iter, int array_len)
{
	DBusMessageIter sub;
	char **result;
	int i;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return NULL;

	result = g_new0(char *, array_len);

	dbus_message_iter_recurse(&iter, &sub);

	i = 0;
	while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
		char *arg;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRING) {
			g_free(result);
			return NULL;
		}

		dbus_message_iter_get_basic(&sub, &arg);

		result[i] = arg;

		i++;
		dbus_message_iter_next(&sub);
	}

	return result;
}

static void query_reply(DBusPendingCall *call, void *user_data)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	struct pending_reply *pending = user_data;
	DBusMessageIter iter, element;
	DBusError derr;
	int err;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		error("Replied with an error: %s, %s", derr.name,
							derr.message);
		dbus_error_free(&derr);

		err = -1;
		goto done;
	}

	dbus_message_iter_init(reply, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
		error("SparqlQuery reply is not an array");

		err = -1;
		goto done;
	}

	dbus_message_iter_recurse(&iter, &element);

	err = 0;

	while (dbus_message_iter_get_arg_type(&element) != DBUS_TYPE_INVALID) {
		char **node;

		if (dbus_message_iter_get_arg_type(&element) !=
						DBUS_TYPE_ARRAY) {
			error("element is not an array");
			goto done;
		}

		node = string_array_from_iter(element, pending->num_fields);
		pending->callback(node, pending->num_fields,
							pending->user_data);

		g_free(node);

		dbus_message_iter_next(&element);
	}

done:
	/* This is the last entry */
	pending->callback(NULL, err, pending->user_data);

	dbus_message_unref(reply);

	/*
	 * pending data is freed in query_free_data after call is unreffed.
	 * Same holds for pending->user_data which is not freed in callback
	 * but in query_free_data.
	 */
}

static void query_free_data(void *user_data)
{
	struct pending_reply *pending = user_data;

	if (!pending)
		return;

	if (pending->destroy)
		pending->destroy(pending->user_data);

	g_free(pending);
}

static DBusPendingCall *query_tracker(const char *query, int num_fields,
				reply_list_foreach_t callback, void *user_data,
				GDestroyNotify destroy, int *err)
{
	struct pending_reply *pending;
	DBusPendingCall *call;
	DBusMessage *msg;

	if (connection == NULL)
		connection = obex_dbus_get_connection();

	msg = dbus_message_new_method_call(TRACKER_SERVICE,
			TRACKER_RESOURCES_PATH, TRACKER_RESOURCES_INTERFACE,
								"SparqlQuery");

	dbus_message_append_args(msg, DBUS_TYPE_STRING, &query,
						DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(connection, msg, &call,
							-1) == FALSE) {
		error("Could not send dbus message");
		dbus_message_unref(msg);
		if (err)
			*err = -EPERM;
		/* user_data is freed otherwise only if call was sent */
		g_free(user_data);
		return NULL;
	}

	pending = g_new0(struct pending_reply, 1);
	pending->callback = callback;
	pending->user_data = user_data;
	pending->num_fields = num_fields;
	pending->destroy = destroy;

	dbus_pending_call_set_notify(call, query_reply, pending,
							query_free_data);
	dbus_message_unref(msg);

	if (err)
		*err = 0;

	return call;
}

static char *iso8601_utc_to_localtime(const char *datetime)
{
	time_t time;
	struct tm tm, *local;
	char localdate[32];
	char tz;
	int nr;

	memset(&tm, 0, sizeof(tm));

	nr = sscanf(datetime, "%04u-%02u-%02uT%02u:%02u:%02u%c",
			&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
			&tm.tm_hour, &tm.tm_min, &tm.tm_sec,
			&tz);
	if (nr < 6) {
		/* Invalid time format */
		error("sscanf(): %s (%d)", strerror(errno), errno);
		return g_strdup("");
	}

	/* Time already in localtime */
	if (nr == 6) {
		strftime(localdate, sizeof(localdate), "%Y%m%dT%H%M%S", &tm);
		return g_strdup(localdate);
	}

	tm.tm_year -= 1900;	/* Year since 1900 */
	tm.tm_mon--;		/* Months since January, values 0-11 */

	time = mktime(&tm);
	time -= timezone;

	local = localtime(&time);

	strftime(localdate, sizeof(localdate), "%Y%m%dT%H%M%S", local);

	return g_strdup(localdate);
}

static void set_call_type(struct phonebook_contact *contact,
				const char *datetime, const char *is_sent,
				const char *is_answered)
{
	gboolean sent, answered;

	if (g_strcmp0(datetime, "NOTACALL") == 0) {
		contact->calltype = CALL_TYPE_NOT_A_CALL;
		return;
	}

	sent = g_str_equal(is_sent, "true");
	answered = g_str_equal(is_answered, "true");

	if (sent == FALSE) {
		if (answered == FALSE)
			contact->calltype = CALL_TYPE_MISSED;
		else
			contact->calltype = CALL_TYPE_INCOMING;
	} else
		contact->calltype = CALL_TYPE_OUTGOING;

	/* Tracker gives time in the ISO 8601 format, UTC time */
	contact->datetime = iso8601_utc_to_localtime(datetime);
}

static struct phonebook_contact *find_contact(GSList *contacts, const char *id)
{
	GSList *l;

	for (l = contacts; l; l = l->next) {
		struct contact_data *c_data = l->data;
		if (g_strcmp0(c_data->id, id) == 0)
			return c_data->contact;
	}

	return NULL;
}

static struct phonebook_number *find_phone(GSList *numbers, const char *phone,
								int type)
{
	GSList *l;

	for (l = numbers; l; l = l->next) {
		struct phonebook_number *pb_num = l->data;
		/* Returning phonebook number if phone values and type values
		 * are equal */
		if (g_strcmp0(pb_num->tel, phone) == 0 && pb_num->type == type)
			return pb_num;
	}

	return NULL;
}

static void add_phone_number(struct phonebook_contact *contact,
						const char *phone, int type)
{
	struct phonebook_number *number;

	if (phone == NULL || strlen(phone) == 0)
		return;

	/* Not adding number if there is already added with the same value */
	if (find_phone(contact->numbers, phone, type))
		return;

	number = g_new0(struct phonebook_number, 1);
	number->tel = g_strdup(phone);
	number->type = type;

	contact->numbers = g_slist_append(contact->numbers, number);
}

static struct phonebook_email *find_email(GSList *emails, const char *address,
								int type)
{
	GSList *l;

	for (l = emails; l; l = l->next) {
		struct phonebook_email *email = l->data;
		if (g_strcmp0(email->address, address) == 0 &&
						email->type == type)
			return email;
	}

	return NULL;
}

static void add_email(struct phonebook_contact *contact, const char *address,
								int type)
{
	struct phonebook_email *email;

	if (address == NULL || strlen(address) == 0)
		return;

	/* Not adding email if there is already added with the same value */
	if (find_email(contact->emails, address, type))
		return;

	email = g_new0(struct phonebook_email, 1);
	email->address = g_strdup(address);
	email->type = type;

	contact->emails = g_slist_append(contact->emails, email);
}

static struct phonebook_address *find_address(GSList *addresses,
					const char *address, int type)
{
	GSList *l;

	for (l = addresses; l; l = l->next) {
		struct phonebook_address *addr = l->data;
		if (g_strcmp0(addr->addr, address) == 0 &&
						addr->type == type)
			return addr;
	}

	return NULL;
}

static void add_address(struct phonebook_contact *contact,
					const char *address, int type)
{
	struct phonebook_address *addr;

	if (address == NULL || address_fields_present(address) == FALSE)
		return;

	/* Not adding address if there is already added with the same value */
	if (find_address(contact->addresses, address, type))
		return;

	addr = g_new0(struct phonebook_address, 1);

	addr->addr = g_strdup(address);
	addr->type = type;

	contact->addresses = g_slist_append(contact->addresses, addr);
}

static GString *gen_vcards(GSList *contacts,
					const struct apparam_field *params)
{
	GSList *l;
	GString *vcards;

	vcards = g_string_new(NULL);

	/* Generating VCARD string from contacts and freeing used contacts */
	for (l = contacts; l; l = l->next) {
		struct contact_data *c_data = l->data;
		phonebook_add_contact(vcards, c_data->contact,
					params->filter, params->format);

		g_free(c_data->id);
		phonebook_contact_free(c_data->contact);
		g_free(c_data);
	}

	return vcards;
}

static void pull_contacts_size(char **reply, int num_fields, void *user_data)
{
	struct phonebook_data *data = user_data;

	if (num_fields < 0) {
		data->cb(NULL, 0, num_fields, 0, data->user_data);
		return;
	}

	if (reply != NULL) {
		data->index = atoi(reply[0]);
		return;
	}

	data->cb(NULL, 0, data->index, 0, data->user_data);

	/*
	 * phonebook_data is freed in query_free_data after call is unreffed.
	 * It is accessible by pointer from data (pending) associated to call.
	 * Useful in cases when call was terminated.
	 */
}

static void add_affiliation(char **field, const char *value)
{
	if (strlen(*field) > 0 || value == NULL || strlen(value) == 0)
		return;

	g_free(*field);

	*field = g_strdup(value);
}

static void contact_init(struct phonebook_contact *contact, char **reply)
{

	contact->fullname = g_strdup(reply[COL_FULL_NAME]);
	contact->family = g_strdup(reply[COL_FAMILY_NAME]);
	contact->given = g_strdup(reply[COL_GIVEN_NAME]);
	contact->additional = g_strdup(reply[COL_ADDITIONAL_NAME]);
	contact->prefix = g_strdup(reply[COL_NAME_PREFIX]);
	contact->suffix = g_strdup(reply[COL_NAME_SUFFIX]);
	contact->birthday = g_strdup(reply[COL_BIRTH_DATE]);
	contact->nickname = g_strdup(reply[COL_NICKNAME]);
	contact->website = g_strdup(reply[COL_URL]);
	contact->photo = g_strdup(reply[COL_PHOTO]);
	contact->company = g_strdup(reply[COL_ORG_NAME]);
	contact->department = g_strdup(reply[COL_ORG_DEPARTMENT]);
	contact->role = g_strdup(reply[COL_ORG_ROLE]);
	contact->uid = g_strdup(reply[COL_UID]);
	contact->title = g_strdup(reply[COL_TITLE]);

	set_call_type(contact, reply[COL_DATE], reply[COL_SENT],
							reply[COL_ANSWERED]);
}

static enum phonebook_number_type get_phone_type(const char *affilation)
{
	if (g_strcmp0(AFFILATION_HOME, affilation) == 0)
		return TEL_TYPE_HOME;
	else if (g_strcmp0(AFFILATION_WORK, affilation) == 0)
		return TEL_TYPE_WORK;

	return TEL_TYPE_OTHER;
}

static void add_main_number(struct phonebook_contact *contact, char *pnumber)
{
	char **num_parts;
	char *type, *number;

	/* For phone taken directly from contacts data, phone number string
	 * is represented as number type and number string - those strings are
	 * separated by SUB_DELIM string */
	num_parts = g_strsplit(pnumber, SUB_DELIM, 2);

	if (!num_parts)
		return;

	if (num_parts[0])
		type = num_parts[0];
	else
		goto failed;

	if (num_parts[1])
		number = num_parts[1];
	else
		goto failed;

	if (g_strrstr(type, FAX_NUM_TYPE))
		add_phone_number(contact, number, TEL_TYPE_FAX);
	else if (g_strrstr(type, MOBILE_NUM_TYPE))
		add_phone_number(contact, number, TEL_TYPE_MOBILE);
	else
		add_phone_number(contact, number, TEL_TYPE_OTHER);

failed:
	g_strfreev(num_parts);
}

static void contact_add_numbers(struct phonebook_contact *contact,
								char **reply)
{
	char **aff_numbers, **con_numbers;
	int i;

	/* Filling phonegit  numbers from contact's affilation */
	aff_numbers = g_strsplit(reply[COL_PHONE_AFF], MAIN_DELIM, MAX_FIELDS);

	if (aff_numbers)
		for(i = 0;aff_numbers[i]; ++i)
			add_phone_number(contact, aff_numbers[i],
					get_phone_type(reply[COL_AFF_TYPE]));

	g_strfreev(aff_numbers);

	/* Filling phone numbers directly from contact's struct */
	con_numbers = g_strsplit(reply[COL_PHONE_CONTACT], MAIN_DELIM,
								MAX_FIELDS);

	if (con_numbers)
		for(i = 0; con_numbers[i] != NULL; ++i)
			add_main_number(contact, con_numbers[i]);

	g_strfreev(con_numbers);
}

static enum phonebook_email_type get_email_type(const char *affilation)
{
	if (g_strcmp0(AFFILATION_HOME, affilation) == 0)
		return EMAIL_TYPE_HOME;
	else if (g_strcmp0(AFFILATION_WORK, affilation) == 0)
		return EMAIL_TYPE_WORK;

	return EMAIL_TYPE_OTHER;
}

static void contact_add_emails(struct phonebook_contact *contact,
								char **reply)
{
	char **aff_emails, **con_emails;
	int i;

	/* Emails from affilation */
	aff_emails = g_strsplit(reply[COL_EMAIL_AFF], MAIN_DELIM, MAX_FIELDS);

	if (aff_emails)
		for(i = 0; aff_emails[i] != NULL; ++i)
			add_email(contact, aff_emails[i],
					get_email_type(reply[COL_AFF_TYPE]));

	g_strfreev(aff_emails);

	/* Emails taken directly from contact's data have always type OTHER */
	con_emails = g_strsplit(reply[COL_EMAIL_CONTACT], MAIN_DELIM,
								MAX_FIELDS);

	if (con_emails)
		for(i = 0; con_emails[i] != NULL; ++i)
			add_email(contact, con_emails[i], EMAIL_TYPE_OTHER);

	g_strfreev(con_emails);

}

static enum phonebook_address_type get_addr_type(const char *affilation)
{
	if (g_strcmp0(AFFILATION_HOME, affilation) == 0)
		return ADDR_TYPE_HOME;
	else if (g_strcmp0(AFFILATION_WORK, affilation) == 0)
		return ADDR_TYPE_WORK;

	return ADDR_TYPE_HOME;
}

static void contact_add_addresses(struct phonebook_contact *contact,
								char **reply)
{

	char **aff_addr, **con_addr;
	int i;

	/* Addresses from affilation */
	aff_addr = g_strsplit(reply[COL_ADDR_AFF], MAIN_DELIM,
								MAX_FIELDS);

	if (aff_addr)
		for(i = 0; aff_addr[i] != NULL; ++i)
			add_address(contact, aff_addr[i],
					get_addr_type(reply[COL_AFF_TYPE]));

	g_strfreev(aff_addr);

	/* Addresses from contact struct */
	con_addr = g_strsplit(reply[COL_ADDR_CONTACT], MAIN_DELIM,
								MAX_FIELDS);

	if (con_addr)
		for(i = 0; con_addr[i] != NULL; ++i)
			add_address(contact, con_addr[i], ADDR_TYPE_OTHER);

	g_strfreev(con_addr);

}

static void contact_add_organization(struct phonebook_contact *contact,
								char **reply)
{
	/* Adding fields connected by nco:hasAffiliation - they may be in
	 * separate replies */
	add_affiliation(&contact->title, reply[COL_TITLE]);
	add_affiliation(&contact->company, reply[COL_ORG_NAME]);
	add_affiliation(&contact->department, reply[COL_ORG_DEPARTMENT]);
	add_affiliation(&contact->role, reply[COL_ORG_ROLE]);
}

static void pull_contacts(char **reply, int num_fields, void *user_data)
{
	struct phonebook_data *data = user_data;
	const struct apparam_field *params = data->params;
	struct phonebook_contact *contact;
	struct contact_data *contact_data;
	GString *vcards;
	int last_index, i;
	gboolean cdata_present = FALSE;
	static char *temp_id = NULL;

	if (num_fields < 0) {
		data->cb(NULL, 0, num_fields, 0, data->user_data);
		goto fail;
	}

	DBG("reply %p", reply);

	if (reply == NULL)
		goto done;

	/* Trying to find contact in recently added contacts. It is needed for
	 * contacts that have more than one telephone number filled */
	contact = find_contact(data->contacts, reply[CONTACTS_ID_COL]);

	/* If contact is already created then adding only new phone numbers */
	if (contact) {
		cdata_present = TRUE;
		goto add_numbers;
	}

	/* We are doing a PullvCardEntry, no need for those checks */
	if (data->vcardentry)
		goto add_entry;

	/* Last four fields are always present, ignoring them */
	for (i = 0; i < num_fields - 4; i++) {
		if (reply[i][0] != '\0')
			break;
	}

	if (i == num_fields - 4 && !g_str_equal(reply[CONTACTS_ID_COL],
						TRACKER_DEFAULT_CONTACT_ME))
		return;

	if (g_strcmp0(temp_id, reply[CONTACTS_ID_COL])) {
		data->index++;
		g_free(temp_id);
		temp_id = g_strdup(reply[CONTACTS_ID_COL]);
	}

	last_index = params->liststartoffset + params->maxlistcount;

	if ((data->index <= params->liststartoffset ||
						data->index > last_index) &&
						params->maxlistcount > 0)
		return;

add_entry:
	contact = g_new0(struct phonebook_contact, 1);
	contact_init(contact, reply);

add_numbers:
	contact_add_numbers(contact, reply);
	contact_add_emails(contact, reply);
	contact_add_addresses(contact, reply);
	contact_add_organization(contact, reply);

	DBG("contact %p", contact);

	/* Adding contacts data to wrapper struct - this data will be used to
	 * generate vcard list */
	if (!cdata_present) {
		contact_data = g_new0(struct contact_data, 1);
		contact_data->contact = contact;
		contact_data->id = g_strdup(reply[CONTACTS_ID_COL]);
		data->contacts = g_slist_append(data->contacts, contact_data);
	}

	return;

done:
	vcards = gen_vcards(data->contacts, params);

	if (num_fields == 0)
		data->cb(vcards->str, vcards->len,
					g_slist_length(data->contacts), 0,
					data->user_data);

	g_string_free(vcards, TRUE);
fail:
	g_slist_free(data->contacts);
	g_free(temp_id);
	temp_id = NULL;

	/*
	 * phonebook_data is freed in query_free_data after call is unreffed.
	 * It is accessible by pointer from data (pending) associated to call.
	 * Useful in cases when call was terminated.
	 */
}

static void add_to_cache(char **reply, int num_fields, void *user_data)
{
	struct cache_data *cache = user_data;
	char *formatted;
	int i;

	if (reply == NULL || num_fields < 0)
		goto done;

	/* the first element is the URI, always not empty */
	for (i = 1; i < num_fields; i++) {
		if (reply[i][0] != '\0')
			break;
	}

	if (i == num_fields &&
			!g_str_equal(reply[0], TRACKER_DEFAULT_CONTACT_ME))
		return;

	if (i == 6)
		formatted = g_strdup(reply[6]);
	else
		formatted = g_strdup_printf("%s;%s;%s;%s;%s",
					reply[1], reply[2], reply[3], reply[4],
					reply[5]);

	/* The owner vCard must have the 0 handle */
	if (strcmp(reply[0], TRACKER_DEFAULT_CONTACT_ME) == 0)
		cache->entry_cb(reply[0], 0, formatted, "",
						reply[6], cache->user_data);
	else
		cache->entry_cb(reply[0], PHONEBOOK_INVALID_HANDLE, formatted,
					"", reply[6], cache->user_data);

	g_free(formatted);

	return;

done:
	if (num_fields <= 0)
		cache->ready_cb(cache->user_data);

	/*
	 * cache is freed in query_free_data after call is unreffed.
	 * It is accessible by pointer from data (pending) associated to call.
	 * Useful in cases when call was terminated.
	 */
}

int phonebook_init(void)
{
	return 0;
}

void phonebook_exit(void)
{
}

char *phonebook_set_folder(const char *current_folder, const char *new_folder,
						uint8_t flags, int *err)
{
	char *tmp1, *tmp2, *base, *path = NULL;
	gboolean root, child;
	int ret = 0;
	int len;

	root = (g_strcmp0("/", current_folder) == 0);
	child = (new_folder && strlen(new_folder) != 0);

	switch (flags) {
	case 0x02:
		/* Go back to root */
		if (!child) {
			path = g_strdup("/");
			goto done;
		}

		path = g_build_filename(current_folder, new_folder, NULL);
		break;
	case 0x03:
		/* Go up 1 level */
		if (root) {
			/* Already root */
			path = g_strdup("/");
			goto done;
		}

		/*
		 * Removing one level of the current folder. Current folder
		 * contains AT LEAST one level since it is not at root folder.
		 * Use glib utility functions to handle invalid chars in the
		 * folder path properly.
		 */
		tmp1 = g_path_get_basename(current_folder);
		tmp2 = g_strrstr(current_folder, tmp1);
		len = tmp2 - (current_folder + 1);

		g_free(tmp1);

		if (len == 0)
			base = g_strdup("/");
		else
			base = g_strndup(current_folder, len);

		/* Return: one level only */
		if (!child) {
			path = base;
			goto done;
		}

		path = g_build_filename(base, new_folder, NULL);
		g_free(base);

		break;
	default:
		ret = -EBADR;
		break;
	}

done:
	if (path && !folder_is_valid(path))
		ret = -ENOENT;

	if (ret < 0) {
		g_free(path);
		path = NULL;
	}

	if (err)
		*err = ret;

	return path;
}

void phonebook_req_finalize(void *request)
{
	struct DBusPendingCall *call = request;

	DBG("");

	if (!dbus_pending_call_get_completed(call))
		dbus_pending_call_cancel(call);

	dbus_pending_call_unref(call);
}

void *phonebook_pull(const char *name, const struct apparam_field *params,
				phonebook_cb cb, void *user_data, int *err)
{
	struct phonebook_data *data;
	const char *query;
	reply_list_foreach_t pull_cb;
	int col_amount;

	DBG("name %s", name);

	if (params->maxlistcount == 0) {
		query = name2count_query(name);
		col_amount = COUNT_QUERY_COL_AMOUNT;
		pull_cb = pull_contacts_size;
	} else {
		query = name2query(name);
		col_amount = PULL_QUERY_COL_AMOUNT;
		pull_cb = pull_contacts;
	}

	if (query == NULL) {
		if (err)
			*err = -ENOENT;
		return NULL;
	}

	data = g_new0(struct phonebook_data, 1);
	data->params = params;
	data->user_data = user_data;
	data->cb = cb;

	return query_tracker(query, col_amount, pull_cb, data, g_free, err);
}

void *phonebook_get_entry(const char *folder, const char *id,
				const struct apparam_field *params,
				phonebook_cb cb, void *user_data, int *err)
{
	struct phonebook_data *data;
	char *query;
	DBusPendingCall *call;

	DBG("folder %s id %s", folder, id);

	data = g_new0(struct phonebook_data, 1);
	data->user_data = user_data;
	data->params = params;
	data->cb = cb;
	data->vcardentry = TRUE;

	if (strncmp(id, CONTACT_ID_PREFIX, strlen(CONTACT_ID_PREFIX)) == 0)
		query = g_strdup_printf(CONTACTS_QUERY_FROM_URI, id, id, id, id,
						id, id, id, id, id, id, id, id,
						id, id, id, id, id, id);
	else
		query = g_strdup_printf(CONTACTS_OTHER_QUERY_FROM_URI,
								id, id, id);

	call = query_tracker(query, PULL_QUERY_COL_AMOUNT, pull_contacts,
							data, g_free, err);

	g_free(query);

	return call;
}

void *phonebook_create_cache(const char *name, phonebook_entry_cb entry_cb,
		phonebook_cache_ready_cb ready_cb, void *user_data, int *err)
{
	struct cache_data *cache;
	const char *query;

	DBG("name %s", name);

	query = folder2query(name);
	if (query == NULL) {
		if (err)
			*err = -ENOENT;
		return NULL;
	}

	cache = g_new0(struct cache_data, 1);
	cache->entry_cb = entry_cb;
	cache->ready_cb = ready_cb;
	cache->user_data = user_data;

	return query_tracker(query, 7, add_to_cache, cache, g_free, err);
}
