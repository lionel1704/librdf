/* -*- Mode: c; c-basic-offset: 2 -*-
 *
 * rdf_query_rasqal.c - RDF Query with Rasqal
 *
 * $Id$
 *
 * Copyright (C) 2004 David Beckett - http://purl.org/net/dajobe/
 * Institute for Learning and Research Technology - http://www.ilrt.bris.ac.uk/
 * University of Bristol - http://www.bristol.ac.uk/
 * 
 * This package is Free Software or Open Source available under the
 * following licenses (these are alternatives):
 *   1. GNU Lesser General Public License (LGPL)
 *   2. GNU General Public License (GPL)
 *   3. Mozilla Public License (MPL)
 * 
 * See LICENSE.html or LICENSE.txt at the top of this package for the
 * full license terms.
 * 
 * 
 */


#include <rdf_config.h>

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h> /* for abort() as used in errors */
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#include <librdf.h>

#include <rasqal.h>




typedef struct
{
  librdf_query *query;        /* librdf query object */
  librdf_model *model;
  rasqal_query *rq;
  char *language;            /* rasqal query language name to use */
  char *query_string;
  librdf_uri *uri;           /* base URI or NULL */

} librdf_query_rasqal_context;


/* prototypes for local functions */
static rasqal_triples_match* rasqal_redland_new_triples_match(rasqal_triples_source *rts, void *user_data, rasqal_triple_meta *m, rasqal_triple *t);
static int rasqal_redland_triple_present(rasqal_triples_source *rts, void *user_data, rasqal_triple *t);
static void rasqal_redland_free_triples_source(void *user_data);


/* functions implementing query api */


static int
librdf_query_rasqal_init(librdf_query* query, 
                         const char *name, librdf_uri* uri,
                         const unsigned char* query_string)
{
  librdf_query_rasqal_context *context=(librdf_query_rasqal_context*)query->context;
  int len;
  unsigned char *query_string_copy;
  
  context->query = query;
  context->language=context->query->factory->name;

  context->rq=rasqal_new_query(context->language, NULL);
  if(!context->rq)
    return 1;

  rasqal_query_set_user_data(context->rq, query);

  len=strlen((const char*)query_string);
  query_string_copy=(unsigned char*)LIBRDF_MALLOC(cstring, len+1);
  if(!query_string_copy)
    return 0;
  strcpy((char*)query_string_copy, (const char*)query_string);

  context->query_string=query_string_copy;
  if(uri)
    context->uri=librdf_new_uri_from_uri(uri);

  return 0;
}


static void
librdf_query_rasqal_terminate(librdf_query* query)
{
  librdf_query_rasqal_context *context=(librdf_query_rasqal_context*)query->context;

  if(context->rq)
    rasqal_free_query(context->rq);

  if(context->query_string)
    LIBRDF_FREE(cstring, context->query_string);

  if(context->uri)
    librdf_free_uri(context->uri);
}


static librdf_node*
rasqal_literal_to_redland_node(librdf_world *world, rasqal_literal* l) {
  if(!l)
    return NULL;
  
  if(l->type == RASQAL_LITERAL_URI)
    return librdf_new_node_from_uri(world, (librdf_uri*)l->value.uri);
  else if (l->type == RASQAL_LITERAL_STRING ||
           l->type == RASQAL_LITERAL_INTEGER ||
           l->type == RASQAL_LITERAL_FLOATING ||
           l->type == RASQAL_LITERAL_BOOLEAN)
    return librdf_new_node_from_typed_literal(world, l->string, 
                                              l->language, 
                                              (librdf_uri*)l->datatype);
  else if (l->type == RASQAL_LITERAL_BLANK)
    return librdf_new_node_from_blank_identifier(world, l->string);
  else {
    LIBRDF_DEBUG2("Could not convert literal type %d to librdf_node", l->type);
    abort();
  }

  return NULL;
}


static rasqal_literal*
redland_node_to_rasqal_literal(librdf_node *node) {
  rasqal_literal* l;
  
  if(librdf_node_is_resource(node)) {
    raptor_uri* uri=(raptor_uri*)librdf_new_uri_from_uri(librdf_node_get_uri(node));
    l=rasqal_new_uri_literal(uri);
  } else if(librdf_node_is_literal(node)) {
    char *string;
    librdf_uri *uri;
    char *new_string;
    char *new_language=NULL;
    raptor_uri *new_datatype=NULL;
    size_t len;
    string=librdf_node_get_literal_value_as_counted_string(node, &len);
    new_string=LIBRDF_MALLOC(cstring, len+1);
    strcpy(new_string, (const char*)string);
    string=librdf_node_get_literal_value_language(node);
    if(string) {
      new_language=LIBRDF_MALLOC(cstring, strlen(string)+1);
      strcpy(new_language, (const char*)string);
    }
    uri=librdf_node_get_literal_value_datatype_uri(node);
    if(uri)
      new_datatype=(raptor_uri*)librdf_new_uri_from_uri(uri);
    l=rasqal_new_string_literal(new_string, new_language, new_datatype, NULL);
  } else {
    char *blank=librdf_node_get_blank_identifier(node);
    char *new_blank=LIBRDF_MALLOC(cstring, strlen(blank)+1);
    strcpy(new_blank, (const char*)blank);
    l=rasqal_new_simple_literal(RASQAL_LITERAL_BLANK, new_blank);
  }

  return l;
}


typedef struct {
  librdf_world *world;
  librdf_query *query;
  librdf_model *model;
} rasqal_redland_triples_source_user_data;



static int
rasqal_redland_new_triples_source(rasqal_query* rdf_query,
                                  void *factory_user_data,
                                  void *user_data,
                                  rasqal_triples_source *rts) {
  librdf_world *world=(librdf_world*)factory_user_data;
  rasqal_redland_triples_source_user_data* rtsc=(rasqal_redland_triples_source_user_data*)user_data;
  raptor_sequence *seq;
  librdf_query_rasqal_context *context;

  seq=rasqal_query_get_source_sequence(rdf_query);
  
  /* FIXME: queries with triple sources are actively discarded */
  if(seq && raptor_sequence_size(seq))
    return 1;

  rtsc->world=world;

  rtsc->query=(librdf_query*)rasqal_query_get_user_data(rdf_query);
  context=(librdf_query_rasqal_context*)rtsc->query->context;
  rtsc->model=context->model;

  rts->new_triples_match=rasqal_redland_new_triples_match;
  rts->triple_present=rasqal_redland_triple_present;
  rts->free_triples_source=rasqal_redland_free_triples_source;

  return 0;
}


static int
rasqal_redland_triple_present(rasqal_triples_source *rts, void *user_data, 
                              rasqal_triple *t) 
{
  rasqal_redland_triples_source_user_data* rtsc=(rasqal_redland_triples_source_user_data*)user_data;
  librdf_node* nodes[3];
  librdf_statement *s;
  int rc;
  
  /* ASSUMPTION: all the parts of the triple are not variables */
  /* FIXME: and no error checks */
  nodes[0]=rasqal_literal_to_redland_node(rtsc->world, t->subject);
  nodes[1]=rasqal_literal_to_redland_node(rtsc->world, t->predicate);
  nodes[2]=rasqal_literal_to_redland_node(rtsc->world, t->object);

  s=librdf_new_statement_from_nodes(rtsc->world, nodes[0], nodes[1], nodes[2]);
  
  rc=librdf_model_contains_statement(rtsc->model, s);
  librdf_free_statement(s);
  return rc;
}



static void
rasqal_redland_free_triples_source(void *user_data) {
  /* rasqal_redland_triples_source_user_data* rtsc=(rasqal_redland_triples_source_user_data*)user_data; */
}


static void
rasqal_redland_register_triples_source_factory(rasqal_triples_source_factory *factory) 
{
  factory->user_data_size=sizeof(rasqal_redland_triples_source_user_data);
  factory->new_triples_source=rasqal_redland_new_triples_source;
}


typedef struct {
  librdf_node* nodes[3];
  /* query statement, made from the nodes above (even when exact) */
  librdf_statement *qstatement;
  librdf_stream *stream;
} rasqal_redland_triples_match_context;


static int
rasqal_redland_bind_match(struct rasqal_triples_match_s* rtm,
                          void *user_data,
                          rasqal_variable* bindings[3]) 
{
  rasqal_redland_triples_match_context* rtmc=(rasqal_redland_triples_match_context*)rtm->user_data;
  librdf_statement* statement=librdf_stream_get_object(rtmc->stream);
  if(!statement)
    return 1;
  
#ifdef RASQAL_DEBUG
  LIBRDF_DEBUG1("  matched statement ");
  librdf_statement_print(statement, stderr);
  fputc('\n', stderr);
#endif

  /* set 1 or 2 variable values from the fields of statement */

  if(bindings[0]) {
    LIBRDF_DEBUG1("binding subject to variable\n");
    rasqal_variable_set_value(bindings[0],
                              redland_node_to_rasqal_literal(librdf_statement_get_subject(statement)));
  }

  if(bindings[1]) {
    if(bindings[0] == bindings[1]) {
      if(!librdf_node_equals(librdf_statement_get_subject(statement),
                             librdf_statement_get_predicate(statement)))
        return 1;
      LIBRDF_DEBUG1("subject and predicate values match\n");
    } else {
      LIBRDF_DEBUG1("binding predicate to variable\n");
      rasqal_variable_set_value(bindings[1], 
                                redland_node_to_rasqal_literal(librdf_statement_get_predicate(statement)));
    }
  }

  if(bindings[2]) {
    int bind=1;
    
    if(bindings[0] == bindings[2]) {
      if(!librdf_node_equals(librdf_statement_get_subject(statement),
                             librdf_statement_get_object(statement)))
        return 1;
      bind=0;
      LIBRDF_DEBUG1("subject and object values match\n");
    }
    if(bindings[1] == bindings[2] &&
       !(bindings[0] == bindings[1]) /* don't do this check if ?x ?x ?x */
       ) {
      if(!librdf_node_equals(librdf_statement_get_predicate(statement),
                             librdf_statement_get_object(statement)))
        return 1;
      bind=0;
      LIBRDF_DEBUG1("predicate and object values match\n");
    }
    
    if(bind) {
      LIBRDF_DEBUG1("binding object to variable\n");
      rasqal_variable_set_value(bindings[2], 
                                redland_node_to_rasqal_literal(librdf_statement_get_object(statement)));
    }
  }

  return 0;
}


static void
rasqal_redland_next_match(struct rasqal_triples_match_s* rtm,
                          void *user_data)
{
  rasqal_redland_triples_match_context* rtmc=(rasqal_redland_triples_match_context*)rtm->user_data;

  librdf_stream_next(rtmc->stream);
}

static int
rasqal_redland_is_end(struct rasqal_triples_match_s* rtm,
                      void *user_data)
{
  rasqal_redland_triples_match_context* rtmc=(rasqal_redland_triples_match_context*)rtm->user_data;

  return librdf_stream_end(rtmc->stream);
}


static void
rasqal_redland_finish_triples_match(struct rasqal_triples_match_s* rtm,
                                    void *user_data) {
  rasqal_redland_triples_match_context* rtmc=(rasqal_redland_triples_match_context*)rtm->user_data;

  if(rtmc->stream) {
    librdf_free_stream(rtmc->stream);
    rtmc->stream=NULL;
  }
  librdf_free_statement(rtmc->qstatement);
  LIBRDF_FREE(rasqal_redland_triples_match_context, rtmc);
}


static rasqal_triples_match*
rasqal_redland_new_triples_match(rasqal_triples_source *rts, void *user_data,
                                 rasqal_triple_meta *m, rasqal_triple *t) {
  rasqal_redland_triples_source_user_data* rtsc=(rasqal_redland_triples_source_user_data*)user_data;
  rasqal_triples_match *rtm;
  rasqal_redland_triples_match_context* rtmc;
  rasqal_variable* var;

  rtm=(rasqal_triples_match *)LIBRDF_CALLOC(rasqal_triples_match, sizeof(rasqal_triples_match), 1);
  rtm->bind_match=rasqal_redland_bind_match;
  rtm->next_match=rasqal_redland_next_match;
  rtm->is_end=rasqal_redland_is_end;
  rtm->finish=rasqal_redland_finish_triples_match;

  rtmc=(rasqal_redland_triples_match_context*)LIBRDF_CALLOC(rasqal_redland_triples_match_context, sizeof(rasqal_redland_triples_match_context), 1);

  rtm->user_data=rtmc;


  /* at least one of the triple terms is a variable and we need to
   * do a triplesMatching() aka librdf_model_find_statements
   *
   * redland find_statements will do the right thing and internally
   * pick the most efficient, indexed way to get the answer.
   */

  if((var=rasqal_literal_as_variable(t->subject))) {
    if(var->value)
      rtmc->nodes[0]=rasqal_literal_to_redland_node(rtsc->world, var->value);
    else
      rtmc->nodes[0]=NULL;
  } else
    rtmc->nodes[0]=rasqal_literal_to_redland_node(rtsc->world, t->subject);

  m->bindings[0]=var;
  

  if((var=rasqal_literal_as_variable(t->predicate))) {
    if(var->value)
      rtmc->nodes[1]=rasqal_literal_to_redland_node(rtsc->world, var->value);
    else
      rtmc->nodes[1]=NULL;
  } else
    rtmc->nodes[1]=rasqal_literal_to_redland_node(rtsc->world, t->predicate);

  m->bindings[1]=var;
  

  if((var=rasqal_literal_as_variable(t->object))) {
    if(var->value)
      rtmc->nodes[2]=rasqal_literal_to_redland_node(rtsc->world, var->value);
    else
      rtmc->nodes[2]=NULL;
  } else
    rtmc->nodes[2]=rasqal_literal_to_redland_node(rtsc->world, t->object);

  m->bindings[2]=var;
  

  rtmc->qstatement=librdf_new_statement_from_nodes(rtsc->world, 
                                                   rtmc->nodes[0],
                                                   rtmc->nodes[1], 
                                                   rtmc->nodes[2]);
  if(!rtmc->qstatement)
    return NULL;

#ifdef RASQAL_DEBUG
  LIBRDF_DEBUG1("query statement: ");
  librdf_statement_print(rtmc->qstatement, stderr);
  fputc('\n', stderr);
#endif
  
  rtmc->stream=librdf_model_find_statements(rtsc->model, rtmc->qstatement);

  LIBRDF_DEBUG1("rasqal_new_triples_match done\n");

  return rtm;
}


static int
librdf_query_rasqal_run_as_bindings(librdf_query* query, librdf_model* model)
{
  librdf_query_rasqal_context *context=(librdf_query_rasqal_context*)query->context;
  context->model = model;

  /* This assumes raptor's URI implementation is librdf_uri */
  if(rasqal_query_prepare(context->rq, context->query_string, 
                          (raptor_uri*)context->uri))
    return 1;

  return rasqal_query_execute(context->rq);
}


static int
librdf_query_rasqal_get_result_count(librdf_query *query)
{
  librdf_query_rasqal_context *context=(librdf_query_rasqal_context*)query->context;
  return rasqal_query_get_result_count(context->rq);
}


static int
librdf_query_rasqal_results_finished(librdf_query *query)
{
  librdf_query_rasqal_context *context=(librdf_query_rasqal_context*)query->context;
  return rasqal_query_results_finished(context->rq);
}


static int
librdf_query_rasqal_get_result_bindings(librdf_query *query, 
                                        const char ***names, 
                                        librdf_node **values)
{
  librdf_query_rasqal_context *context=(librdf_query_rasqal_context*)query->context;
  int size=raptor_sequence_size(rasqal_query_get_variable_sequence(context->rq));
  rasqal_literal **literals;
  int rc;
  int i;

  if(values) {
    rc=rasqal_query_get_result_bindings(context->rq, names, &literals);
  } else
    rc=rasqal_query_get_result_bindings(context->rq, names, NULL);

  if(rc || !values)
    return rc;

  for(i=0; i<size; i++)
    values[i]=rasqal_literal_to_redland_node(query->world, literals[i]);

  return 0;
}


static librdf_node*
librdf_query_rasqal_get_result_binding_value(librdf_query *query, int offset)
{
  librdf_query_rasqal_context *context=(librdf_query_rasqal_context*)query->context;
  rasqal_literal* literal;

  literal=rasqal_query_get_result_binding_value(context->rq, offset);

  return rasqal_literal_to_redland_node(query->world, literal);
}


static const char*
librdf_query_rasqal_get_result_binding_name(librdf_query *query, int offset)
{
  librdf_query_rasqal_context *context=(librdf_query_rasqal_context*)query->context;
  return rasqal_query_get_result_binding_name(context->rq, offset);
}


static librdf_node*
librdf_query_rasqal_get_result_binding_by_name(librdf_query *query, const char *name)
{
  librdf_query_rasqal_context *context=(librdf_query_rasqal_context*)query->context;
  rasqal_literal* literal;

  literal=rasqal_query_get_result_binding_by_name(context->rq, name);

  return rasqal_literal_to_redland_node(query->world, literal);
}


static int
librdf_query_rasqal_next_result(librdf_query *query)
{
  librdf_query_rasqal_context *context=(librdf_query_rasqal_context*)query->context;
  return rasqal_query_next_result(context->rq);
}


/* local function to register list query functions */

static void
librdf_query_rasqal_register_factory(librdf_query_factory *factory) 
{
  factory->context_length     = sizeof(librdf_query_rasqal_context);
  
  factory->init               = librdf_query_rasqal_init;
  factory->terminate          = librdf_query_rasqal_terminate;
  factory->run_as_bindings            = librdf_query_rasqal_run_as_bindings;
  factory->get_result_count           = librdf_query_rasqal_get_result_count;
  factory->results_finished           = librdf_query_rasqal_results_finished;
  factory->get_result_bindings        = librdf_query_rasqal_get_result_bindings;
  factory->get_result_binding_value   = librdf_query_rasqal_get_result_binding_value;
  factory->get_result_binding_name    = librdf_query_rasqal_get_result_binding_name;
  factory->get_result_binding_by_name = librdf_query_rasqal_get_result_binding_by_name;
  factory->next_result                = librdf_query_rasqal_next_result;
}


void
librdf_query_rasqal_constructor(librdf_world *world)
{
  librdf_query_register_factory(world, "rdql", NULL,
                                &librdf_query_rasqal_register_factory);

  rasqal_init();
  
  rasqal_set_triples_source_factory(rasqal_redland_register_triples_source_factory, world);
}


void
librdf_query_rasqal_destructor(void)
{
  rasqal_finish();
}