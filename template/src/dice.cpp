/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#include <eosiolib/crypto.h>
#include <eosiolib/asset.hpp>
#include <eosiolib/contract.hpp>
#include <eosiolib/eosio.hpp>
#include <eosiolib/fixed_bytes.hpp>
#include <eosiolib/time.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace std;
using namespace eosio;

#define N(X) name(#X)
#define CORE_SYMBOL symbol("SYS", 4)

CONTRACT dice : public contract {
   public:
    const uint32_t FIVE_MINUTES = 5 * 60;

    dice(name receiver, name code, datastream<const char*> ds)
        : contract(receiver, code, ds),
          offers(receiver, receiver.value),
          games(receiver, receiver.value),
          global_dices(receiver, receiver.value),
          accounts(receiver, receiver.value) {}

    //@abi action
    ACTION offerbet(const asset& bet, const name player,
                    const checksum256& commitment) {
        eosio_assert(bet.symbol == CORE_SYMBOL, "only core token allowed");
        eosio_assert(bet.is_valid(), "invalid bet");
        eosio_assert(bet.amount > 0, "must bet positive quantity");

        eosio_assert(!has_offer(commitment),
                     "offer with this commitment already exist");
        require_auth(player);

        auto cur_player_itr = accounts.find(player.value);
        eosio_assert(cur_player_itr != accounts.end(), "unknown account");

        // Store new offer
        auto new_offer_itr = offers.emplace(_self, [&](auto& offer) {
            offer.id = offers.available_primary_key();
            offer.bet = bet;
            offer.owner = player;
            offer.commitment = commitment;
            offer.gameid = 0;
        });

        // Try to find a matching bet
        auto idx = offers.template get_index<N(bet)>();
        auto matched_offer_itr =
            idx.lower_bound((uint64_t)new_offer_itr->bet.amount);

        if (matched_offer_itr == idx.end() ||
            matched_offer_itr->bet != new_offer_itr->bet ||
            matched_offer_itr->owner == new_offer_itr->owner) {
            // No matching bet found, update player's account
            accounts.modify(cur_player_itr, _self, [&](auto& acnt) {
                eosio_assert(acnt.eos_balance >= bet, "insufficient balance");
                acnt.eos_balance -= bet;
                acnt.open_offers++;
            });

        } else {
            // Create global game counter if not exists
            auto gdice_itr = global_dices.begin();
            if (gdice_itr == global_dices.end()) {
                gdice_itr = global_dices.emplace(
                    _self, [&](auto& gdice) { gdice.nextgameid = 0; });
            }

            // Increment global game counter
            global_dices.modify(gdice_itr, _self,
                                [&](auto& gdice) { gdice.nextgameid++; });

            // Create a new game
            auto game_itr = games.emplace(_self, [&](auto& new_game) {
                new_game.id = gdice_itr->nextgameid;
                new_game.bet = new_offer_itr->bet;
                new_game.deadline = eosio::time_point_sec(0);

                new_game.player1.commitment = matched_offer_itr->commitment;
                memset(&new_game.player1.reveal, 0, sizeof(checksum256));

                new_game.player2.commitment = new_offer_itr->commitment;
                memset(&new_game.player2.reveal, 0, sizeof(checksum256));
            });

            // Update player's offers
            idx.modify(matched_offer_itr, _self, [&](auto& offer) {
                offer.bet.amount = 0;
                offer.gameid = game_itr->id;
            });

            offers.modify(new_offer_itr, _self, [&](auto& offer) {
                offer.bet.amount = 0;
                offer.gameid = game_itr->id;
            });

            // Update player's accounts
            accounts.modify(accounts.find(matched_offer_itr->owner.value),
                            _self, [&](auto& acnt) {
                                acnt.open_offers--;
                                acnt.open_games++;
                            });

            accounts.modify(cur_player_itr, _self, [&](auto& acnt) {
                eosio_assert(acnt.eos_balance >= bet, "insufficient balance");
                acnt.eos_balance -= bet;
                acnt.open_games++;
            });
        }
    }

    //@abi action
    ACTION canceloffer(const checksum256& commitment) {
        auto idx = offers.template get_index<N(commitment)>();
        auto offer_itr = idx.find(offer::get_commitment(commitment));

        eosio_assert(offer_itr != idx.end(), "offer does not exists");
        eosio_assert(offer_itr->gameid == 0, "unable to cancel offer");
        require_auth(offer_itr->owner);

        auto acnt_itr = accounts.find(offer_itr->owner.value);
        accounts.modify(acnt_itr, _self, [&](auto& acnt) {
            acnt.open_offers--;
            acnt.eos_balance += offer_itr->bet;
        });

        idx.erase(offer_itr);
    }

    //@abi action
    ACTION reveal(const checksum256& commitment, const checksum256& source) {
        // assert_sha256((char*)&source, sizeof(source),
        //               (const checksum256*)&commitment);

        auto idx = offers.template get_index<N(commitment)>();
        auto curr_revealer_offer = idx.find(offer::get_commitment(commitment));

        eosio_assert(curr_revealer_offer != idx.end(), "offer not found");
        eosio_assert(curr_revealer_offer->gameid > 0, "unable to reveal");

        auto game_itr = games.find(curr_revealer_offer->gameid);

        player curr_reveal = game_itr->player1;
        player prev_reveal = game_itr->player2;

        if (!is_equal(curr_reveal.commitment, commitment)) {
            std::swap(curr_reveal, prev_reveal);
        }

        eosio_assert(is_zero(curr_reveal.reveal) == true,
                     "player already revealed");

        if (!is_zero(prev_reveal.reveal)) {
            capi_checksum256 result;
            sha256((char*)&game_itr->player1, sizeof(player) * 2, &result);

            auto prev_revealer_offer =
                idx.find(offer::get_commitment(prev_reveal.commitment));

            int winner = result.hash[1] < result.hash[0] ? 0 : 1;

            if (winner) {
                pay_and_clean(*game_itr, *curr_revealer_offer,
                              *prev_revealer_offer);
            } else {
                pay_and_clean(*game_itr, *prev_revealer_offer,
                              *curr_revealer_offer);
            }

        } else {
            games.modify(game_itr, _self, [&](auto& game) {
                if (is_equal(curr_reveal.commitment, game.player1.commitment))
                    game.player1.reveal = source;
                else
                    game.player2.reveal = source;

                game.deadline = eosio::time_point_sec(now() + FIVE_MINUTES);
            });
        }
    }

    //@abi action
    ACTION claimexpired(const uint64_t gameid) {
        auto game_itr = games.find(gameid);

        eosio_assert(game_itr != games.end(), "game not found");
        eosio_assert(game_itr->deadline != eosio::time_point_sec(0) &&
                         eosio::time_point_sec(now()) > game_itr->deadline,
                     "game not expired");

        auto idx = offers.template get_index<N(commitment)>();
        auto player1_offer =
            idx.find(offer::get_commitment(game_itr->player1.commitment));
        auto player2_offer =
            idx.find(offer::get_commitment(game_itr->player2.commitment));

        if (!is_zero(game_itr->player1.reveal)) {
            eosio_assert(is_zero(game_itr->player2.reveal), "game error");
            pay_and_clean(*game_itr, *player1_offer, *player2_offer);
        } else {
            eosio_assert(is_zero(game_itr->player1.reveal), "game error");
            pay_and_clean(*game_itr, *player2_offer, *player1_offer);
        }
    }

    //@abi action
    ACTION deposit(const name from, const asset& quantity) {
        eosio_assert(quantity.is_valid(), "invalid quantity");
        eosio_assert(quantity.amount > 0, "must deposit positive quantity");

        auto itr = accounts.find(from.value);
        if (itr == accounts.end()) {
            itr = accounts.emplace(_self, [&](auto& acnt) {
                acnt.owner = from;
                acnt.eos_balance = asset(0, CORE_SYMBOL);
            });
        }

        action(permission_level{from, N(active)}, N(eosio.token), N(transfer),
               std::make_tuple(from, _self, quantity, std::string("")))
            .send();

        accounts.modify(itr, _self,
                        [&](auto& acnt) { acnt.eos_balance += quantity; });
    }

    //@abi action
    ACTION withdraw(const name to, const asset& quantity) {
        require_auth(to);

        eosio_assert(quantity.is_valid(), "invalid quantity");
        eosio_assert(quantity.amount > 0, "must withdraw positive quantity");

        auto itr = accounts.find(to.value);
        eosio_assert(itr != accounts.end(), "unknown account");

        accounts.modify(itr, _self, [&](auto& acnt) {
            eosio_assert(acnt.eos_balance >= quantity, "insufficient balance");
            acnt.eos_balance -= quantity;
        });

        action(permission_level{_self, N(active)}, N(eosio.token), N(transfer),
               std::make_tuple(_self, to, quantity, std::string("")))
            .send();

        if (itr->is_empty()) {
            accounts.erase(itr);
        }
    }

    using offerbet_action = action_wrapper<"offerbet"_n, &dice::offerbet>;
    using canceloffer_action =
        action_wrapper<"canceloffer"_n, &dice::canceloffer>;
    using reveal_action = action_wrapper<"reveal"_n, &dice::reveal>;
    using claimexpired_action =
        action_wrapper<"claimexpired"_n, &dice::claimexpired>;
    using deposit_action = action_wrapper<"deposit"_n, &dice::deposit>;
    using withdraw_action = action_wrapper<"withdraw"_n, &dice::withdraw>;

   private:
    //@abi table offer i64
    struct offer {
        uint64_t id;
        name owner;
        asset bet;
        checksum256 commitment;
        uint64_t gameid = 0;

        uint64_t primary_key() const { return id; }

        uint64_t by_bet() const { return (uint64_t)bet.amount; }

        checksum256 by_commitment() const { return get_commitment(commitment); }

        static checksum256 get_commitment(const checksum256& commitment) {
            const uint64_t* p64 =
                reinterpret_cast<const uint64_t*>(&commitment);
            return checksum256::make_from_word_sequence<uint64_t>(
                p64[0], p64[1], p64[2], p64[3]);
        }

        EOSLIB_SERIALIZE(offer, (id)(owner)(bet)(commitment)(gameid))
    };

    typedef eosio::multi_index<
        "offer"_n, offer,
        indexed_by<"bet"_n, const_mem_fun<offer, uint64_t, &offer::by_bet> >,
        indexed_by<"commitment"_n,
                   const_mem_fun<offer, checksum256, &offer::by_commitment> > >
        offer_index;

    struct player {
        checksum256 commitment;
        checksum256 reveal;

        EOSLIB_SERIALIZE(player, (commitment)(reveal))
    };

    //@abi table game i64
    TABLE game {
        uint64_t id;
        asset bet;
        eosio::time_point_sec deadline;
        player player1;
        player player2;

        uint64_t primary_key() const { return id; }

        EOSLIB_SERIALIZE(game, (id)(bet)(deadline)(player1)(player2))
    };

    typedef eosio::multi_index<"game"_n, game> game_index;

    //@abi table global i64
    TABLE global_dice {
        uint64_t id = 0;
        uint64_t nextgameid = 0;

        uint64_t primary_key() const { return id; }

        EOSLIB_SERIALIZE(global_dice, (id)(nextgameid))
    };

    typedef eosio::multi_index<N(global), global_dice> global_dice_index;

    //@abi table account i64
    TABLE account {
        account(name o = name()) : owner(o) {}

        name owner;
        asset eos_balance;
        uint32_t open_offers = 0;
        uint32_t open_games = 0;

        bool is_empty() const {
            return !(eos_balance.amount | open_offers | open_games);
        }

        uint64_t primary_key() const { return owner.value; }

        EOSLIB_SERIALIZE(account, (owner)(eos_balance)(open_offers)(open_games))
    };

    typedef eosio::multi_index<N(account), account> account_index;

    offer_index offers;
    game_index games;
    global_dice_index global_dices;
    account_index accounts;

    bool has_offer(const checksum256& commitment) const {
        auto idx = offers.template get_index<N(commitment)>();
        auto itr = idx.find(offer::get_commitment(commitment));
        return itr != idx.end();
    }

    bool is_equal(const checksum256& a, const checksum256& b) const {
        return memcmp((void*)&a, (const void*)&b, sizeof(checksum256)) == 0;
    }

    bool is_zero(const checksum256& a) const {
        const uint64_t* p64 = reinterpret_cast<const uint64_t*>(&a);
        return p64[0] == 0 && p64[1] == 0 && p64[2] == 0 && p64[3] == 0;
    }

    void pay_and_clean(const game& g, const offer& winner_offer,
                       const offer& loser_offer) {
        // Update winner account balance and game count
        auto winner_account = accounts.find(winner_offer.owner.value);
        accounts.modify(winner_account, _self, [&](auto& acnt) {
            acnt.eos_balance += 2 * g.bet;
            acnt.open_games--;
        });

        // Update losser account game count
        auto loser_account = accounts.find(loser_offer.owner.value);
        accounts.modify(loser_account, _self,
                        [&](auto& acnt) { acnt.open_games--; });

        if (loser_account->is_empty()) {
            accounts.erase(loser_account);
        }

        games.erase(g);
        offers.erase(winner_offer);
        offers.erase(loser_offer);
    }
};

EOSIO_DISPATCH(dice,
               (offerbet)(canceloffer)(reveal)(claimexpired)(deposit)(withdraw))
