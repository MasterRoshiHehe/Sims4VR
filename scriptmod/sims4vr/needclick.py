"""First person: press a need (wrist HUD or the phone's Needs app) and the game
finds something that fulfils it (user 2026-10-28 14:23Z).

This is the game's own button on a need (SimInfoMotivePanel CommodityContainer
.OnSolveButtonClick -> PostServerCommand "stats.solve_motive" with the
commodity; server_commands/statistic_commands.py solve_motive): a FullAutonomy
AutonomyRequest for that one commodity, consider_scores_of_zero, posture
behaviour IGNORE_SI_STATE, is_script_request, no opportunity cost, label
"AutoSolveMotive". The best action found is executed, or the commodity's
commodity_autosolve_failure_interaction when none is.

Differences from the game's command, so that you do the walking yourself:
- the request's context is SCRIPT_WITH_USER_INTENT (you asked), not
  AUTONOMY, so scripted.py's autonomy block lets it through and the guided
  trail (trail.py, halt._picked) leads you to the object instead of the Sim
  walking there;
- nothing is retried and nothing else is pushed.

Switch: [body] need_click (default on; debug menu Trail).
"""
from sims4vr import native


def _log(message):
    native._log('needclick: ' + message)


def solve(index):
    """Need `index` (hudinfo.need_commodities order) was pressed."""
    try:
        if not native.setting_flag('body', 'need_click', True):
            return
        import services
        from sims4vr import halt, hudinfo
        sim = services.get_active_sim()
        if sim is None or not halt.first_person(sim):
            return
        if not 0 <= index < len(hudinfo.need_commodities):
            return
        commodity = hudinfo.need_commodities[index]
        _solve(sim, commodity)
    except Exception as e:
        native._log('needclick: error {!r}'.format(e))


def _solve(sim, commodity):
    import autonomy.autonomy_modes
    import autonomy.autonomy_request
    from autonomy.autonomy_request import AutonomyPostureBehavior
    from interactions import priority
    from interactions.aop import AffordanceObjectPair
    from interactions.context import InteractionContext, InteractionBucketType
    if sim.commodity_tracker is None:
        return
    stat = sim.commodity_tracker.get_statistic(commodity)
    if stat is None:
        _log('{}: no such need on the Sim'.format(commodity))
        return
    if not sim.queue.can_queue_visible_interaction():
        _log('the queue is full')
        return
    context = InteractionContext(sim, InteractionContext.SOURCE_SCRIPT_WITH_USER_INTENT, priority.Priority.High,
                                 bucket=InteractionBucketType.DEFAULT)
    request = autonomy.autonomy_request.AutonomyRequest(
        sim, autonomy_mode=autonomy.autonomy_modes.FullAutonomy, commodity_list=[stat], context=context,
        consider_scores_of_zero=True, posture_behavior=AutonomyPostureBehavior.IGNORE_SI_STATE,
        is_script_request=True, allow_opportunity_cost=False, autonomy_mode_label_override='AutoSolveMotive')
    import services
    selected = services.autonomy_service().find_best_action(request)
    if selected is None:
        failure = commodity.commodity_autosolve_failure_interaction
        if failure is not None and not sim.queue.has_duplicate_super_affordance(failure, sim, None):
            AffordanceObjectPair(failure, None, failure, None).test_and_execute(context)
        _log('{}: nothing found to fulfil it'.format(commodity.__name__))
        return
    if sim.queue.has_duplicate_super_affordance(selected.affordance, sim, selected.target):
        _log('{}: already in your queue'.format(selected))
        return
    source = getattr(getattr(selected, 'context', None), 'source', None)
    if source != InteractionContext.SOURCE_SCRIPT_WITH_USER_INTENT:
        # The game made it with its own context: make it yours.
        result = sim.push_super_affordance(selected.affordance, selected.target, context,
                                           **getattr(selected, 'interaction_parameters', {}))
        _log('{}: pushed as your pick ({})'.format(selected, bool(result)))
        return
    result = AffordanceObjectPair.execute_interaction(selected)
    _log('{} for {}: {}'.format(selected, commodity.__name__, bool(result)))
