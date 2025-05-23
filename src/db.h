/******************************************************************************
  Copyright (c) 1992, 1995, 1996 Xerox Corporation.  All rights reserved.
  Portions of this code were written by Stephen White, aka ghond.
  Use and copying of this software and preparation of derivative works based
  upon this software are permitted.  Any distribution of this software or
  derivative works must comply with all applicable United States export
  control laws.  This software is made available AS IS, and Xerox Corporation
  makes no warranty about the software, its performance or its conformity to
  any specification.  Any person obtaining a copy of this software is requested
  to send their name and post office or electronic mail address to:
    Pavel Curtis
    Xerox PARC
    3333 Coyote Hill Rd.
    Palo Alto, CA 94304
    Pavel@Xerox.Com
 *****************************************************************************/

#ifndef DB_h
#define DB_h 1

#include "config.h"
#include "program.h"
#include "structures.h"


/**** file system ****/

extern const char *db_usage_string(void);
				/* Returns a string describing the database
				 * command-line arguments.
				 */

extern int db_initialize(int *pargc, char ***pargv);
				/* (*pargc) and (*pargv) refer to the database
				 * command-line arguments and perhaps others.
				 * Returns true (and sets (*pargc) and (*pargv)
				 * to refer to any extra arguments) if the
				 * database args were valid.
				 */

extern void db_recoverymode(int set_on);
				/* Turn contents/child list rebuilding on.
				 */

extern int db_load(void);
				/* Does any necessary long-running preparations
				 * of the database, such as loading significant
				 * amounts of data into memory, returning true
				 * iff this operation is successful.
				 */

enum db_flush_type {
    FLUSH_IF_FULL,		/* Do output only if there's `too much' changed
				 * data in memory now. */
    FLUSH_ONE_SECOND,		/* Do output for up to about one second. */
    FLUSH_ALL_NOW,		/* Do all pending output, forking the server
				 * if necessary. */
    FLUSH_PANIC			/* Do all pending output; the server is going
				 * down in emergency mode. */
};

extern int db_flush(enum db_flush_type);
				/* Flush some amount of the changed portion of
				 * the database to disk, as indicated by the
				 * argument.  Returns true on success.
				 */

extern int32 db_disk_size(void);
				/* Return the total size, in bytes, of the most
				 * recent full representation of the database
				 * as one or more disk files.  Returns -1 if,
				 * for some reason, no such on-disk
				 * representation is currently available.
				 */

extern void db_shutdown(void);
				/* Shut down the database module, flushing all
				 * pending database changes to disk and only
				 * returning after this is done.
				 */

/**** objects ****/

extern Objid db_create_object(void);
				/* Creates a new object with parent & location
				 * == #-1.  Returns new object's id number.
				 */

extern Objid db_last_used_objid(void);
extern void db_reset_last_used_objid(void);
				/* The former returns the highest object number
				 * ever returned by db_create_object().  The
				 * latter resets that high-water mark to the
				 * highest object number referring to a
				 * currently-valid object.
				 */

extern void db_destroy_object(Objid);
				/* Destroys object, freeing all associated
				 * storage.  The object's parent and location
				 * must == #-1 and it must not have any
				 * children or contents.
				 */

extern Objid db_renumber_object(Objid);
				/* Renumbers object to have the lowest free
				 * object number.  Returns its new number.
				 */

extern int valid(Objid);

extern int db_object_bytes(Objid);
				/* Returns the number of bytes of memory
				 * currently required in order to represent the
				 * given object and all of its verbs and
				 * properties.
				 */

/**** object attributes ****/

extern Objid db_object_owner(Objid);
extern void db_set_object_owner(Objid oid, Objid owner);

extern const char *db_object_name(Objid);
extern void db_set_object_name(Objid oid, const char *name);
				/* These functions do not change the reference
				 * count of the name they accept/return.  Thus,
				 * the caller should str_ref() the name if the
				 * reference is to be persistent.
				 */

extern Objid db_object_parent(Objid);
extern int db_count_children(Objid);
extern int db_for_all_children(Objid,
			       int (*)(void *, Objid),
			       void *);
				/* The outcome is unspecified if any of the
				 * following functions are called during a call
				 * to db_for_all_children():
				 *      db_create_object()
				 *      db_destroy_object()
				 *      db_renumber_object()
				 *      db_change_parent()
				 */
extern int db_change_parent(Objid oid, Objid parent);
				/* db_change_parent() returns true (and
				 * actually changes the parent of OID) iff
				 * neither OID nor any of its descendents
				 * defines any properties with the same names
				 * as properties defined on PARENT or any of
				 * its ancestors.
				 */

extern Objid db_object_location(Objid);
extern int db_count_contents(Objid);
extern int db_for_all_contents(Objid,
			       int (*)(void *, Objid),
			       void *);
				/* The outcome is unspecified if any of the
				 * following functions are called during a call
				 * to db_for_all_contects():
				 *      db_destroy_object()
				 *      db_renumber_object()
				 *      db_change_location()
				 */
extern void db_change_location(Objid oid, Objid location);

extern Objid db_first_child(Objid);
extern Objid db_last_child(Objid);

extern Objid db_first_contents(Objid);
extern Objid db_last_contents(Objid);
extern void db_object_occupants(Objid oid_what, Objid oid_parent, Var* v);

/* NOTE: New flags must always be added to the end of this list, rather than
 *     replacing one of the obsolete ones, since old databases might have
 *       old objects around that still have that flag set.
 */
typedef enum {
    FLAG_USER, FLAG_PROGRAMMER, FLAG_WIZARD, FLAG_OBSOLETE_1,
    FLAG_READ, FLAG_WRITE, FLAG_OBSOLETE_2, FLAG_FERTILE
} db_object_flag;

extern int db_object_has_flag(Objid, db_object_flag);
extern void db_set_object_flag(Objid, db_object_flag);
extern void db_clear_object_flag(Objid, db_object_flag);

extern int db_object_allows(Objid oid, Objid progr,
			    db_object_flag flag);
				/* Returns true iff either OID has FLAG or
				 * PROGR either is a wizard or owns OID; that
				 * is, iff PROGR's authority is sufficient to
				 * be allowed to do the indicated operation on
				 * OID.
				 */

extern int is_wizard(Objid oid);
extern int is_programmer(Objid oid);
extern int is_user(Objid oid);
extern int is_a(Objid oid_what, Objid oid_parent);
extern int is_in(Objid oid_what, Objid oid_where);
extern int is_in_a(Objid oid_what, Objid oid_type_of_location);

extern Var db_all_users(void);
				/* db_all_users() does not change the reference
				 * count of the `users' list maintained by this
				 * module.  The caller should thus var_ref() it
				 * to make it persistent.
				 */


/**** properties *****/

typedef enum {
    PF_READ = 0x1,
    PF_WRITE = 0x2,
    PF_CHOWN = 0x4,
    PF_PRIVATE = 0x8
} db_prop_flag;

extern int db_add_propdef(Objid oid, const char *pname,
			  Var value, Objid owner,
			  unsigned flags);
				/* Returns true (and actually adds the property
				 * to OID) iff (1) no property named PNAME
				 * already exists on OID or one of its
				 * ancestors or descendants, and (2) PNAME does
				 * not name any built-in property.  This
				 * function does not change the reference count
				 * of VALUE, so the caller should var_ref() it
				 * if the caller's reference is to persist.
				 * FLAGS should be the result of ORing together
				 * zero or more elements of `db_prop_flag'.
				 */

extern int db_rename_propdef(Objid oid, const char *old,
			     const char *new);
				/* Returns true (and actually renames the
				 * propdef on OID) iff (1) a propdef with the
				 * name OLD existed on OID, (2) no property
				 * named NEW already exists on OID or one of
				 * its ancestors or descendants, and (3) NEW
				 * does not name any built-in property.  If
				 * condition (1) holds and OLD == NEW, then
				 * this is a no-op that returns true.
				 */

extern int db_delete_propdef(Objid, const char *);
				/* Returns true iff a propdef with the given
				 * name existed on the object (i.e., was there
				 * to be deleted).
				 */

extern int db_count_propdefs(Objid);
extern int db_for_all_propdefs(Objid,
			       int (*)(void *, const char *),
			       void *);
				/* db_for_all_propdefs() does not change the
				 * reference counts of the property names
				 * passed to the given callback function.
				 * Thus, the caller should str_ref() the names
				 * if the references are to be persistent.
				 */

enum bi_prop {
    BP_NONE = 0,
    BP_NAME, BP_OWNER,
    BP_PROGRAMMER, BP_WIZARD,
    BP_R, BP_W, BP_F,
    BP_LOCATION, BP_CONTENTS
};

typedef struct {
    enum bi_prop built_in;	/* true iff property is a built-in one */
    Objid definer;		/* if !built_in, the object defining prop */
    void *ptr;			/* null iff property not found */
} db_prop_handle;

extern db_prop_handle db_find_property(Objid oid, const char *name,
				       Var * value);
				/* Returns a handle on the named property of
				 * the given object.  If `value' is non-null,
				 * then the value of the named property (after
				 * skipping over any `clear' slots up the
				 * ancestor list) will be stored through it.
				 * For non-built-in properties, the reference
				 * count of the value is not changed by this
				 * function, so the caller should var_ref() it
				 * if it is to be persistent; some built-in
				 * properties have freshly generated values, so
				 * the caller should free_var() the value in
				 * the built-in case if it is *not* to be
				 * persistent.  The `ptr' in the result of this
				 * function will be null iff there is no such
				 * property on the object.  The `built_in'
				 * field in the result is true iff the named
				 * property is one built into all objects, in
				 * which case it specifies which such property
				 * it is.  The returned handle is very
				 * volatile; only the routines declared
				 * immediately below as taking a
				 * `db_prop_handle' argument are guaranteed to
				 * leave the handle intact.
				 */

extern Var db_property_value(db_prop_handle);
extern void db_set_property_value(db_prop_handle, Var);
				/* For non-built-in properties, these functions
				 * do not change the reference count of the
				 * value they accept/return, so the caller
				 * should var_ref() the value if it is to be
				 * persistent.  Some built-in properties have
				 * freshly generated values, so the caller
				 * should free_var() the value in the built-in
				 * case if it is *not* to be persistent.
				 * The server will panic if
				 * `db_set_property_value()' is called with an
				 * illegal value for a built-in property.
				 */

extern Objid db_property_owner(db_prop_handle);
extern void db_set_property_owner(db_prop_handle, Objid);
				/* These functions may not be called for
				 * built-in properties.
				 */

extern unsigned db_property_flags(db_prop_handle);
extern void db_set_property_flags(db_prop_handle, unsigned);
				/* The property flags are the result of ORing
				 * together zero or more elements of
				 * `db_prop_flag', defined above.  These
				 * functions may not be called for built-in
				 * properties.
				 */

extern int db_property_allows(db_prop_handle, Objid,
			      db_prop_flag);
				/* Returns true iff either the property has the
				 * flag or the object either is a wizard or
				 * owns the property; that is, iff the object's
				 * authority is sufficient to be allowed to do
				 * the indicated operation on the property.
				 * This function may not be called for built-in
				 * properties.
				 */


/**** verbs ****/

typedef enum {
    VF_READ = 01,
    VF_WRITE = 02,
    VF_EXEC = 04,
    VF_DEBUG = 010
} db_verb_flag;

typedef enum {
    ASPEC_NONE, ASPEC_ANY, ASPEC_THIS
} db_arg_spec;

typedef enum {
    PREP_ANY = -2,
    PREP_NONE = -1,
    Other_Preps = 0		/* Others are indices into DB-internal table */
} db_prep_spec;

extern db_prep_spec db_find_prep(int argc, char *argv[],
				 int *first, int *last);
				/* Look for a prepositional phrase in the
				 * ARGC-element sequence ARGV, returning
				 * PREP_NONE if none was found.  If FIRST and
				 * LAST are both non-null, then the indices
				 * of the first and last word in the phrase
				 * are stored through them.  If either FIRST
				 * or LAST is null, then the match will fail
				 * unless the entire sequence is a phrase.
				 */

extern db_prep_spec db_match_prep(const char *);
				/* Try to recognize the given string as a
				 * prepositional phrase, returning the
				 * appropriate db_prep_spec on success and
				 * PREP_NONE on failure.  The string may be of
				 * the form
				 *      [#]<digit>+
				 * (an optional hash mark followed by one or
				 * more decimal digits).  If the number so
				 * represented is a legitimate index for a
				 * prepositional phrase in the DB-internal
				 * table, the corresponding db_prep_spec is
				 * returned, or PREP_NONE otherwise.
				 */

extern const char *db_unparse_prep(db_prep_spec);
				/* Returns a string giving the human-readable
				 * name(s) for the given preposition
				 * specification.  The returned string may not
				 * be dynamically allocated, so the caller
				 * should str_dup() it if it is to be
				 * persistent.
				 */

extern void db_add_verb(Objid oid, const char *vnames,
			Objid owner, unsigned flags,
			db_arg_spec dobj, db_prep_spec prep,
			db_arg_spec iobj);
				/* This function does not change the reference
				 * count of the NAMES string it accepts.  Thus,
				 * the caller should str_ref() it if it is to
				 * be persistent.
				 */

extern int db_count_verbs(Objid);
extern int db_for_all_verbs(Objid,
			    int (*)(void *, const char *),
			    void *);
				/* db_for_all_verbs() does not change the
				 * reference counts of the verb names
				 * passed to the given callback function.
				 * Thus, the caller should str_ref() the names
				 * if the references are to be persistent.
				 */

typedef struct {
    void *ptr;
} db_verb_handle;

extern db_verb_handle db_find_command_verb(Objid oid, const char *verb,
					 db_arg_spec dobj, unsigned prep,
					   db_arg_spec iobj);
				/* Returns a handle on the first matching
				 * verb found defined on OID or one of its
				 * ancestors.  A matching verb has a name
				 * matching VERB, a dobj spec equal either to
				 * ASPEC_ANY or DOBJ, a prep spec equal either
				 * to PREP_ANY or PREP, and an iobj spec equal
				 * either to ASPEC_ANY or IOBJ.  The `ptr' in
				 * the result is null iff there is no such
				 * verb.  The returned handle is very volatile;
				 * only the routines declared below as taking a
				 * `db_verb_handle' argument are guaranteed to
				 * leave the handle intact.
				 */

extern db_verb_handle db_find_callable_verb(Objid oid, const char *verb);
				/* Returns a handle on the first verb found
				 * defined on OID or one of its ancestors with
				 * a name matching VERB (and, for now, the
				 * VF_EXEC flag set).  The `ptr' in the result
				 * is null iff there is no such verb.  The
				 * returned handle is very volatile; only the
				 * routines declared below as taking a
				 * `db_verb_handle' argument are guaranteed to
				 * leave the handle intact.
				 */

extern db_verb_handle db_find_defined_verb(Objid oid, const char *verb,
					   int allow_numbers);
				/* Returns a handle on the first verb found
				 * defined on OID with a name matching VERB
				 * (or, if ALLOW_NUMBERS is true and VERB has
				 * the form of a decimal natural number, the
				 * zero-based VERB'th verb defined on OID,
				 * whichever comes first).  The `ptr' in the
				 * result is null iff there is no such verb.
				 * The returned handle is very volatile; only
				 * the routines declared below as taking a
				 * `db_verb_handle' argument are guaranteed to
				 * leave the handle intact.
				 */

extern Var db_find_matching_verb_numbers(Objid oid, const char *vname);
				/* Returns a list of 1-indexed verb numbers
				 * found defined on OID with a name matching
				 * VERB.
				 */

extern db_verb_handle db_find_indexed_verb(Objid oid, unsigned index);
				/* Returns a handle on the 1-based INDEX'th
				 * verb defined on OID.  The `ptr' in the
				 * result is null iff there is no such verb.
				 * The returned handle is very volatile; only
				 * the routines declared below as taking a
				 * `db_verb_handle' argument are guaranteed to
				 * leave the handle intact.
				 */

extern Objid db_verb_definer(db_verb_handle);
				/* Returns the object on which the given verb
				 * is defined.
				 */

extern const char *db_verb_names(db_verb_handle);
extern void db_set_verb_names(db_verb_handle, const char *);
				/* These functions do not change the reference
				 * count of the string they accept/return.
				 * Thus, the caller should str_ref() it if it
				 * is to be persistent.
				 */

extern Objid db_verb_owner(db_verb_handle);
extern void db_set_verb_owner(db_verb_handle, Objid);

extern unsigned db_verb_flags(db_verb_handle);
extern void db_set_verb_flags(db_verb_handle, unsigned);

extern Program *db_verb_program(db_verb_handle);
extern void db_set_verb_program(db_verb_handle, Program *);
				/* These functions do not change the reference
				 * count of the program they accept/return.
				 * Thus, the caller should program_ref() it if
				 * it is to be persistent.
				 */

extern void db_verb_arg_specs(db_verb_handle h,
			      db_arg_spec * dobj,
			      db_prep_spec * prep,
			      db_arg_spec * iobj);
extern void db_set_verb_arg_specs(db_verb_handle h,
				  db_arg_spec dobj,
				  db_prep_spec prep,
				  db_arg_spec iobj);

extern int db_verb_allows(db_verb_handle, Objid, db_verb_flag);
				/* Returns true iff either the verb has the
				 * flag or the object either is a wizard or
				 * owns the verb; that is, iff the object's
				 * authority is sufficient to be allowed to do
				 * the indicated operation on the verb.
				 */

extern void db_delete_verb(db_verb_handle);
				/* Deletes the given verb entirely.  This
				 * db_verb_handle may not be used again after
				 * this call returns.
				 */

#endif				/* !DB_h */

/* 
 * $Log: db.h,v $
 * Revision 1.9  2010/05/17 01:49:02  blacklite
 * add bf_occupants
 *
 * Revision 1.8  2010/05/16 02:41:03  blacklite
 * Add new first/last_in, first/last_contents builtins and remove outdated TOMB constant. v1.10.4
 *
 * Revision 1.7  2009/07/27 01:45:54  blacklite
 * add is_in_a
 *
 * Revision 1.6  2009/07/22 23:14:23  blacklite
 * add internal is_a and is_in, and moo functions which pass to each.
 *
 * Revision 1.5  2009/03/27 20:24:37  blacklite
 * add PF_PRIVATE flag to props
 *
 * Revision 1.4  2008/08/24 05:06:13  blacklite
 * Add -r option and more fixes to the recovery process.
 *
 * Revision 1.3  2007/09/12 07:33:29  spunky
 * This is a working version of the current HellMOO server
 *
 * Revision 1.3  1998/12/14 13:17:32  nop
 * Merge UNSAFE_OPTS (ref fixups); fix Log tag placement to fit CVS whims
 *
 * Revision 1.2  1997/03/03 04:18:26  nop
 * GNU Indent normalization
 *
 * Revision 1.1.1.1  1997/03/03 03:45:02  nop
 * LambdaMOO 1.8.0p5
 *
 * Revision 2.5  1996/05/12  21:31:37  pavel
 * Added documentation of the memory-management policy for db_add_verb().
 * Release 1.8.0p5.
 *
 * Revision 2.4  1996/02/11  00:47:04  pavel
 * Enhanced db_find_property() to report the defining object of the found
 * property.  Release 1.8.0beta2.
 *
 * Revision 2.3  1996/02/08  06:28:33  pavel
 * Updated copyright notice for 1996.  Release 1.8.0beta1.
 *
 * Revision 2.2  1995/12/28  00:30:27  pavel
 * Added db_disk_size().  Changed db_delete_verb() to unbundle how the verb is
 * found.  Added support to db_find_defined_verb() for suppressing old
 * numeric-string behavior.  Release 1.8.0alpha3.
 *
 * Revision 2.1  1995/12/11  07:56:12  pavel
 * Added `db_object_bytes()' and `db_rename_propdef()'.  Clarified
 * reference-counting behavior of built-in property references.
 *
 * Release 1.8.0alpha2.
 *
 * Revision 2.0  1995/11/30  05:05:11  pavel
 * New baseline version, corresponding to release 1.8.0alpha1.
 */
